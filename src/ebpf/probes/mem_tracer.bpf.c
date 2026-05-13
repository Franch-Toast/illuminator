#include "../include/common.bpf.h"

// ============================================================================
// 文件：illuminator/src/ebpf/probes/mem_tracer.bpf.c
// ============================================================================
//
// 【作用】
// 内存分配/释放追踪 eBPF 探针。通过 uprobe/uretprobe 机制挂载在用户态
// 应用程序的 malloc/free 函数上，追踪每一次内存分配和释放操作。
// 支持内存泄漏检测（通过 allocs map 追踪未释放的分配）。
//
// 【工作原理】
// 1. uprobe/uretprobe 机制：
//    uprobe 在目标函数入口处触发（此时可读取参数），uretprobe 在函数返回时触发。
//    通过此机制可以观察 malloc 的请求大小（入口参数）和返回地址（返回值）。
//
// 2. 三阶段追踪流水线：
//    阶段 1 - uprobe_malloc（入口）：记录请求分配大小到 alloc_sizes map
//    阶段 2 - uretprobe_malloc（返回）：拿到返回地址，构造分配事件
//    阶段 3 - uprobe_free（入口）：匹配之前记录的分配信息，构造释放事件
//
// 3. alloc_sizes 临时存储：
//    key=tid（线程 ID），value=请求大小。
//    这是因为 uprobe 在 entry 看到参数，但返回值只有在 uretprobe 中才能获取。
//    使用 tid 作为 key 保证同一线程的 entry/return 正确配对。
//
// 4. allocs map（未释放分配追踪）：
//    key=分配返回的地址，value=完整的 il_mem_event。
//    分配时：写入 map（标记为 alloc=1）。
//    释放时：查找对应的分配记录（分配的大小），然后从 map 中删除。
//    析构时仍留在 map 中的记录即为潜在的内存泄漏。
//
// 5. 使用限制：
//    - 需要目标进程的 ELF 二进制，且需要函数符号表（非 stripped）
//    - 不支持 mmap/munmap（直接内存映射），仅追踪堆分配器
//    - 线程安全：alloc_sizes 使用 pid_tgid(低 32 位=tid) 做 key 避免跨线程干扰

// ============================================================================
// BPF Maps 定义
// ============================================================================

// mem_events：内存事件 ring buffer
// 每个事件约 56 字节，256KB 可缓冲约 4600 个事件
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} mem_events SEC(".maps");

// allocs：未释放分配追踪表（用于泄漏检测）
// key=分配返回的虚拟地址, value=完整的内存事件快照
// 当 free 被调用时，按此地址查找对应的分配信息
// 程序退出时，该 map 中残留的条目即为潜在的内存泄漏
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);  // 最多追踪 65536 个活跃分配
    __type(key, __u64);
    __type(value, struct il_mem_event);
} allocs SEC(".maps");

// alloc_sizes：临时大小存储（uprobe→uretprobe 参数传递）
// key=tid（线程 ID），value=请求的分配大小
// uprobe_malloc 写入请求大小，uretprobe_malloc 读取后清理
// 使用 tid 而非 pid 作为 key，因为同一进程的不同线程可能并发调用 malloc
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);   // 最多 4096 个线程并发调用 malloc
    __type(key, __u64);
    __type(value, __u64);
} alloc_sizes SEC(".maps");

// ============================================================================
// uprobe_malloc：malloc 入口探针
//
// 功能：在 malloc(size) 被调用时，记录请求分配的大小。
//
// 参数：ctx - 用户态寄存器上下文
//       PT_REGS_PARM1(ctx) = 第一个参数寄存器 = malloc 的 size 参数
//
// 处理：将 (tid → size) 存入 alloc_sizes，供 uretprobe 阶段使用。
//       使用 bpf_get_current_pid_tgid() 的低 32 位作为 tid。
// ============================================================================
SEC("uprobe")
int uprobe_malloc(struct pt_regs *ctx) {
    // 读取 malloc 的第一个参数：请求分配大小
    // PT_REGS_PARM1 宏根据架构自动选择正确的寄存器（x86_64: rdi, arm64: x0）
    __u64 size = PT_REGS_PARM1(ctx);

    // 使用 tid（pid_tgid 的低 32 位）作为临时存储的 key
    __u64 tid = bpf_get_current_pid_tgid();

    // 存入 alloc_sizes，BPF_ANY 允许覆盖（同一线程顺序调用 malloc）
    bpf_map_update_elem(&alloc_sizes, &tid, &size, BPF_ANY);
    return 0;
}

// ============================================================================
// uretprobe_malloc：malloc 返回探针
//
// 功能：在 malloc 返回时，获取实际分配的地址并构造内存分配事件。
//
// 参数：ctx - 用户态寄存器上下文
//       PT_REGS_RC(ctx) = 返回值寄存器 = malloc 返回的堆内存地址
//
// 处理流程：
// 1. 从 alloc_sizes 获取当前线程之前记录的大小
// 2. 获取返回值（分配的地址），如果为 NULL 则分配失败，不记录
// 3. 构造 il_mem_event（alloc=1），填充时间、进程、地址、大小信息
// 4. 同时写入 allocs map（用于后续泄漏追踪）
// 5. 通过 ring buffer 推送事件
// 6. 清理 alloc_sizes 中的临时记录
// ============================================================================
SEC("uretprobe")
int uretprobe_malloc(struct pt_regs *ctx) {
    // 查找当前线程之前记录的分配大小
    __u64 tid = bpf_get_current_pid_tgid();
    __u64 *size = bpf_map_lookup_elem(&alloc_sizes, &tid);
    if (!size) return 0;  // 没有对应的 uprobe 记录（异常情况）

    // 读取 malloc 返回值：分配的堆内存地址
    // 若返回 NULL（分配失败），清理临时记录后退出
    __u64 addr = PT_REGS_RC(ctx);
    if (addr == 0) {
        bpf_map_delete_elem(&alloc_sizes, &tid);
        return 0;
    }

    // 预留 ring buffer 空间
    struct il_mem_event *e = bpf_ringbuf_reserve(&mem_events, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&alloc_sizes, &tid);
        return 0;
    }

    // 填充事件字段
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;       // 进程 ID (tgid)
    e->tid = (__u32)pid_tgid;      // 线程 ID (pid)
    e->addr = addr;                // malloc 返回的堆地址
    e->size = *size;               // 用户请求的分配大小
    e->alloc = 1;                  // 标记为分配事件
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // 将分配记录写入 allocs map，用于后续 free 时的匹配和泄漏检测
    bpf_map_update_elem(&allocs, &addr, e, BPF_ANY);

    // 提交事件并清理临时存储
    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&alloc_sizes, &tid);
    return 0;
}

// ============================================================================
// uprobe_free：free 入口探针
//
// 功能：在 free(ptr) 被调用时，查找之前记录的分配信息并构造释放事件。
//
// 参数：ctx - 用户态寄存器上下文
//       PT_REGS_PARM1(ctx) = free 的第一个参数 = 要释放的内存地址
//
// 处理流程：
// 1. 以释放地址为 key，在 allocs map 中查找之前的分配记录
// 2. 如果找到：构造 il_mem_event（alloc=0），复制原分配的大小信息
// 3. 如果找不到：free(NULL) 或释放的是未被追踪的地址（如 mmap 直接分配的）
// 4. 从 allocs map 中删除记录
//
// 泄漏检测原理：定时扫描 allocs map 中残留的 entry，
// 这些 entry 对应的地址未被 free → 可能的内存泄漏。
// ============================================================================
SEC("uprobe")
int uprobe_free(struct pt_regs *ctx) {
    // 读取 free 的第一个参数：要释放的内存地址
    __u64 addr = PT_REGS_PARM1(ctx);
    if (addr == 0) return 0;  // free(NULL) 是合法操作，忽略

    // 在 allocs map 中查找该地址对应的原始分配信息
    // 如果找不到：可能是通过 mmap 直接分配（未经过 malloc），不追踪
    struct il_mem_event *alloc = bpf_map_lookup_elem(&allocs, &addr);
    if (!alloc) return 0;

    // 预留 ring buffer 空间
    struct il_mem_event *e = bpf_ringbuf_reserve(&mem_events, sizeof(*e), 0);
    if (!e) {
        // ring buffer 满也要清理 map 记录 → 避免 allocs 泄漏
        bpf_map_delete_elem(&allocs, &addr);
        return 0;
    }

    // 填充释放事件字段
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;
    e->tid = (__u32)pid_tgid;
    e->addr = addr;
    e->size = alloc->size;     // 从之前记录的分配事件中复制大小
    e->alloc = 0;              // 标记为释放事件
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // 清理追踪记录并提交事件
    // 注意：必须先 delete 再 submit，因为此时已确认释放了对应地址的内存
    bpf_map_delete_elem(&allocs, &addr);
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
