#include "../include/common.bpf.h"

// ============================================================================
// rodata 配置（由用户态在 skeleton load 前写入）
// ============================================================================
// sample_freq：与 userspace perf_event_attr.sample_freq 保持一致，便于 BPF
// 程序在运行时感知当前采样频率（例如用于阈值计算或校验）。
volatile const __u64 sample_freq = 49;

// ============================================================================
// 文件：illuminator/src/ebpf_common/probes/cpu_profiler.bpf.c
// ============================================================================
//
// 【作用】
// CPU 剖析器 eBPF 探针。通过 perf_event 子系统以固定频率（默认 99Hz）
// 采集每个 CPU 核心上正在运行的进程/线程及其调用栈，生成 CPU 热点火焰图
// 所需的核心数据。
//
// 【工作原理】
// 1. perf_event 触发机制：
//    用户态通过 perf_event_open() 为每个 CPU 核心创建一个定时触发的
//    PERF_COUNT_SW_CPU_CLOCK 事件。eBPF 程序 on_cpu_sample 作为该事件的
//    回调函数，每 1/frequency 秒被调用一次。
//
// 2. 调用栈记录：
//    通过 bpf_get_stackid() 获取当前进程的内核栈和用户栈快照，
//    存入 stacks map（STACK_TRACE 类型）。stack id 作为栈帧内容的引用，
//    避免在 ring buffer 中直接传输大量栈帧数据。
//
// 3. 两级聚合策略：
//    - 聚合层（stack_counts map）：将 (pid, tid, kernel_stack_id, user_stack_id)
//      四元组作为 key，计数为 value。相同调用栈的采样不重复发送 ring buffer 事件。
//    - 流式层（cpu_events ring buffer）：当 cfg_flags bit0=1 时，同时向 ring buffer
//      发送每个采样事件的详细信息，供实时处理链路消费。
//
// 4. 过滤机制（通过 cpu_profiler_cfg 配置图控制）：
//    bit0 = 1 → 启用 ring buffer 实时流模式
//    bit1 = 2 → 启用 PID 白名单过滤（仅追踪 target_pids 中的进程）
//    bit2 = 4 → 启用 comm 白名单过滤（仅追踪 target_comms 中的进程）
//
// 5. 与 cpu_sampler 的区别：
//    cpu_profiler 是"聚合优先"模式，默认只更新 stack_counts map 而不推流；
//    cpu_sampler 是"流式优先"模式，每个采样都写入 ring buffer。
//    cpu_profiler 适用于长期持续剖析（低开销），cpu_sampler 适用于即时诊断。

// ============================================================================
// BPF Maps 定义
// ============================================================================

// cpu_profiler_cfg：探针配置图（单元素数组）
// key=0, value=配置标志位：
//   bit0 = 1 → 实时流模式（ring buffer 事件）
//   bit1 = 2 → PID 白名单过滤
//   bit2 = 4 → 进程名白名单过滤
// 用户态通过 bpf_map_update_elem 动态修改配置
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);  // 数组类型：固定大小，O(1) 查找
    __uint(max_entries, 1);            // 仅一个元素，索引为 0
    __type(key, __u32);
    __type(value, __u32);
} cpu_profiler_cfg SEC(".maps");

// target_pids：PID 白名单哈希表
// key=目标进程 PID, value=占位标记
// 仅在 cfg 的 bit1=1 时生效，未被列入的进程将被静默跳过
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);  // 最多追踪 4096 个进程
    __type(key, __u32);
    __type(value, __u8);
} target_pids SEC(".maps");

// target_comms：进程名白名单哈希表
// key=目标进程名（char[16]）, value=占位标记
// 仅在 cfg 的 bit2=1 时生效
// 注意：键是定长字符数组（TASK_COMM_LEN=16），不是字符串指针
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);   // 最多匹配 256 个进程名
    __type(key, char[TASK_COMM_LEN]);
    __type(value, __u8);
} target_comms SEC(".maps");

// cpu_events：CPU 采样事件 ring buffer
// 环形共享内存队列，BPF 侧写入事件，用户态异步消费
// 仅在 cfg bit0=1 时写入实际数据
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);  // 256KB 容量
} cpu_events SEC(".maps");

// stacks：调用栈存储 map（STACK_TRACE 类型）
// key=stack_id（内部分配）, value=栈帧地址数组
// bpf_get_stackid() 自动去重相同的栈帧序列，返回已有的 stack_id
// max_entries=16384 可存储 1.6 万个不同的调用栈
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));  // 每栈最多 127 帧
    __uint(max_entries, 16384);
} stacks SEC(".maps");

// stack_counts：调用栈聚合计数器
// key=il_stack_key（pid+tid+双栈id+进程名）
// value=该组合出现次数
// 用于累积统计，生成火焰图时使用
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct il_stack_key);
    __type(value, __u64);
} stack_counts SEC(".maps");

// cpu_pidns_cfg：PID Namespace 配置（用于 bpf_get_ns_current_pid_tgid）
// 用户态通过 stat("/proc/self/ns/pid") 获取 dev/ino 并写入此 map
// BPF 使用此信息将 root-ns PID 翻译为 namespace-local PID
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct il_pidns_config);
} cpu_pidns_cfg SEC(".maps");

DECLARE_META_STATS();

// ============================================================================
// on_cpu_sample：CPU 采样处理函数（perf_event 类型）
//
// 功能：处理单次 CPU 定时采样的完整流程，包括过滤、栈记录、聚合计数和事件推送。
//
// 参数：ctx - perf_event 上下文，提供调用栈回溯所需的寄存器信息
// 返回：0（始终成功，跳过的情况也返回 0）
//
// 处理流程：
// 1. 获取当前进程信息（pid, tid）
// 2. 检查配置标志和白名单过滤
// 3. 获取内核栈和用户栈的 stack_id
// 4. 更新聚合计数器（stack_counts map）
// 5. 如果启用流模式，构造并提交 ring buffer 事件
// ============================================================================
SEC("perf_event")
int on_cpu_sample(struct bpf_perf_event_data *ctx) {
    // rodata 配置引用，确保 sample_freq 被链接进 .rodata 并可在 load 前配置。
    // 当前仅作存在性校验：freq=0 表示未正确配置，直接跳过。
    if (sample_freq == 0)
        return 0;

    // ---------------------------------------------------------------
    // 步骤 1：获取当前进程/线程标识
    // bpf_get_current_pid_tgid() 返回 (tgid << 32) | pid 的 64 位值
    // 高 32 位 = tgid（进程 ID），低 32 位 = pid（线程 ID）
    // ---------------------------------------------------------------
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = pid_tgid >> 32;     // 进程 ID（tgid）— root namespace
    __u32 tid = (__u32)pid_tgid;    // 线程 ID（pid）— root namespace

    // 跳过内核空闲任务（pid=0 是 idle task，没有有意义的用户态上下文）
    if (pid == 0)
        return 0;

    // ---------------------------------------------------------------
    // 步骤 1.5：PID Namespace 翻译
    // 如果 cpu_pidns_cfg 有配置，使用 bpf_get_ns_current_pid_tgid()
    // 将 root-ns PID 翻译为目标 namespace 的 local PID
    // 这样 BPF 可以直接使用用户态传入的 namespace-local PID 进行过滤
    // ---------------------------------------------------------------
    __u32 ns_key = 0;
    struct il_pidns_config *ns_cfg = bpf_map_lookup_elem(&cpu_pidns_cfg, &ns_key);
    if (ns_cfg && ns_cfg->ino != 0) {
        struct bpf_pidns_info ns_info = {};
        long ret = bpf_get_ns_current_pid_tgid(
            ns_cfg->dev, ns_cfg->ino, &ns_info, sizeof(ns_info));
        if (ret == 0) {
            pid = ns_info.tgid;
            tid = ns_info.pid;
        }
        // ret != 0: 进程不在目标 namespace 中，使用 root-ns PID
    }

    // ---------------------------------------------------------------
    // 步骤 2：读取配置标志位
    // 默认 cfg_flags=1（仅启用实时流），用户态可动态修改
    // ---------------------------------------------------------------
    __u32 cfg_k = 0;
    __u32 cfg_flags = 1;
    __u32 *cfg_p = bpf_map_lookup_elem(&cpu_profiler_cfg, &cfg_k);
    if (cfg_p)
        cfg_flags = *cfg_p;

    // ---------------------------------------------------------------
    // 步骤 3：PID 白名单过滤（cfg bit1=2）
    // 如果启用了 PID 过滤但当前 pid 不在 target_pids 中，跳过本次采样
    // 此时 pid 已经是 namespace-local PID（如果配置了 pidns）
    // ---------------------------------------------------------------
    if (cfg_flags & 2) {
        if (!bpf_map_lookup_elem(&target_pids, &pid)) {
            INC_STAT(STAT_FILTERED);
            return 0;
        }
    }

    // ---------------------------------------------------------------
    // 步骤 4：进程名白名单过滤（cfg bit2=4）
    // 获取当前进程名，检查是否在 target_comms 中
    // ---------------------------------------------------------------
    if (cfg_flags & 4) {
        char cur_comm[TASK_COMM_LEN];
        bpf_get_current_comm(&cur_comm, sizeof(cur_comm));
        if (!bpf_map_lookup_elem(&target_comms, &cur_comm)) {
            INC_STAT(STAT_FILTERED);
            return 0;
        }
    }

    // ---------------------------------------------------------------
    // 步骤 5：获取调用栈
    // BPF_F_FAST_STACK_CMP 标志启用更高效的栈帧比较（跳过帧指针校验）
    // 第一次调用获取内核栈，第二次获取用户栈（FP | BPF_F_USER_STACK）
    // stack_id=-1 表示获取失败（如无栈帧信息或权限不够）
    // ---------------------------------------------------------------
    __s32 kernel_stack_id =
        bpf_get_stackid(ctx, &stacks, BPF_F_FAST_STACK_CMP);
    __s32 user_stack_id =
        bpf_get_stackid(ctx, &stacks, BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

    // ---------------------------------------------------------------
    // 步骤 6：更新聚合计数器
    // 构造聚合键（pid + tid + 双栈 id + 进程名）
    // 使用原子加 __sync_fetch_and_add 更新计数（此 map 可能被多 CPU 并发访问）
    // 如果键不存在，插入初始值 1（BPF_NOEXIST 避免覆盖）
    // ---------------------------------------------------------------
    struct il_stack_key key = {};
    key.pid = pid;
    key.tid = tid;
    key.kernel_stack_id = kernel_stack_id;
    key.user_stack_id = user_stack_id;
    bpf_get_current_comm(&key.comm, sizeof(key.comm));

    __u64 *count = bpf_map_lookup_elem(&stack_counts, &key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 init = 1;
        bpf_map_update_elem(&stack_counts, &key, &init, BPF_NOEXIST);
    }

    // ---------------------------------------------------------------
    // 步骤 7：如果未启用流模式（bit0=0），到此结束
    // 聚合模式的好处：减少 ring buffer 压力，适合长期持续运行
    // ---------------------------------------------------------------
    if (!(cfg_flags & 1))
        return 0;

    // ---------------------------------------------------------------
    // 步骤 8：流模式：构造 CPU 采样事件并通过 ring buffer 推送
    // bpf_ringbuf_reserve() 从 ring buffer 中预留事件大小的空间
    // 如果 ring buffer 满（返回 NULL），静默丢弃本次采样
    // bpf_ringbuf_submit() 提交事件，使其对用户态可见
    // ---------------------------------------------------------------
    INC_STAT(STAT_TOTAL_EVENTS);
    struct il_cpu_sample_event *e =
        bpf_ringbuf_reserve(&cpu_events, sizeof(*e), 0);
    if (!e) {
        INC_STAT(STAT_BUFFER_FULL);
        return 0;
    }

    e->timestamp_ns = bpf_ktime_get_ns();  // 纳秒级时间戳
    e->pid = pid;
    e->tid = tid;
    e->cpu = bpf_get_smp_processor_id();   // 当前 CPU 核心编号
    bpf_get_current_comm(&e->comm, sizeof(e->comm));
    e->kernel_stack_id = kernel_stack_id;
    e->user_stack_id = user_stack_id;

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// ============================================================================
// BPF 许可证声明
// 内核要求所有 BPF 程序声明许可证，GPL 兼容的 helper 函数需要此声明
// ============================================================================
char LICENSE[] SEC("license") = "GPL";
