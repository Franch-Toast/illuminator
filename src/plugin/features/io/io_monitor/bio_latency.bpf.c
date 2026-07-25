#include "../include/common.bpf.h"

// ============================================================================
// 文件：illuminator/src/ebpf_common/probes/bio_latency.bpf.c
// ============================================================================
//
// 【作用】
// 块 IO（Block IO）延迟追踪 eBPF 探针。通过挂载在 block 子系统的
// tracepoint 上，精确测量每次磁盘 IO 请求从提交（issue）到完成（complete）
// 的完整延迟时间。用于定位磁盘 IO 瓶颈和慢查询诊断。
//
// 【工作原理】
// 1. 双 tracepoint 配对方案：
//    - trace_block_rq_issue（block_rq_issue）：
//      在 IO 请求被提交到块设备驱动队列时触发。记录：时间戳、发起进程信息、请求标识。
//    - trace_block_rq_complete（block_rq_complete）：
//      在 IO 请求完成（数据已传输或已写入）时触发。取出之前记录的开始时间，
//      计算延迟 = 完成时间 - 提交时间。
//
// 2. 请求关联机制：
//    通过 req_starts hash map 存储未完成的 IO 请求。
//    key=sector（起始扇区号），value=ns_request_start（开始时间+进程信息）。
//    之所以使用 sector 而非 request 指针作为 key：
//    - sector 是固定大小整数，更适合 BPF map key
//    - 同一时刻同一扇区通常只有一个活跃 IO 请求
//    - 避免了通过指针追踪内核对象的安全问题
//
// 3. 读写区分：
//    通过检查 ctx->rwbs 字段的第一个字符判断 IO 类型：
//    rwbs[0]=='W' → 写操作（rwflag=1），否则 → 读操作（rwflag=0）
//
// 4. 延迟分桶统计（用户态侧）：
//    BPF 只提供原始延迟数据（latency_ns），延迟分桶和百分位数计算
//    由用户态数据分析引擎完成。

// ============================================================================
// BPF Maps 定义
// ============================================================================

// bio_events：块 IO 事件 ring buffer
// 每个 IO 完成事件约 56 字节，256KB 可缓冲约 4600 个事件
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} bio_events SEC(".maps");

// ---------------------------------------------------------------------------
// ns_request_start：IO 请求开始时的快照信息
// 包含开始时间、发起进程/线程 ID 和进程名称
// ---------------------------------------------------------------------------
struct ns_request_start {
    __u64 start_ns;         // IO 提交时间戳（纳秒）
    __u32 pid;              // 发起 IO 的进程 ID（tgid）
    __u32 tid;              // 发起 IO 的线程 ID（pid）
    char comm[TASK_COMM_LEN]; // 进程名称
};

// req_starts：未完成 IO 请求追踪表
// key   = __u64 起始扇区号（ctx->sector）
// value = ns_request_start 开始快照
// 当 IO 完成时按 key 查找对应的开始信息，计算延迟后删除
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);  // 最多同时追踪 16384 个未完成 IO 请求
    __type(key, __u64);
    __type(value, struct ns_request_start);
} req_starts SEC(".maps");

DECLARE_COLLECTION_GATE();
DECLARE_META_STATS();

// ============================================================================
// trace_block_rq_issue：IO 请求提交 tracepoint 处理函数
//
// 功能：在 IO 请求被提交到块设备队列时记录开始快照信息。
//
// 参数：ctx - block_rq_issue tracepoint 的上下文
//       ctx->sector：IO 请求的起始扇区号
//       ctx->rwbs：读写标志字符串（"R"=读, "W"=写, "D"=丢弃等）
//
// 处理流程：
// 1. 以 ctx->sector 为 key，构造 ns_request_start 结构体
// 2. 记录当前时间戳（bpf_ktime_get_ns()）
// 3. 记录当前进程/线程 ID 和进程名
// 4. 存入 req_starts map，使用 BPF_ANY 允许覆盖（理论上同扇区不应有并发请求）
// ============================================================================
SEC("tracepoint/block/block_rq_issue")
int trace_block_rq_issue(struct trace_event_raw_block_rq *ctx) {
    __u64 key = ctx->sector;
    struct ns_request_start start = {};
    start.start_ns = bpf_ktime_get_ns();

    // 获取当前进程/线程信息（内核中执行 submit_bio 的上下文）
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    start.pid = pid_tgid >> 32;
    start.tid = (__u32)pid_tgid;
    bpf_get_current_comm(&start.comm, sizeof(start.comm));

    // 写入追踪表，BPF_ANY 允许覆盖（安全策略：最后一个请求覆盖之前的记录）
    bpf_map_update_elem(&req_starts, &key, &start, BPF_ANY);
    return 0;
}

// ============================================================================
// trace_block_rq_complete：IO 请求完成 tracepoint 处理函数
//
// 功能：在 IO 请求完成时计算延迟并推送事件。
//
// 参数：ctx - block_rq_complete tracepoint 的上下文
//       ctx->sector：IO 请求的起始扇区号
//       ctx->nr_sector：IO 请求扇区数
//       ctx->rwbs：读写标志字符串
//
// 处理流程：
// 1. 以 ctx->sector 为 key 查找 req_starts map 中的开始快照
// 2. 如果找不到（可能是内核直接在内存缓存完成，未经过 issue tracepoint），跳过
// 3. 计算延迟：now - start.start_ns
// 4. 构造 il_bio_event 并通过 ring buffer 推送
// 5. 从 req_starts 中删除已完成的请求记录（防止 map 泄漏）
//
// 注意事项：
// - 如果 ring buffer 预留失败（满），start 记录仍会被删除，避免 map 膨胀
// - rwfs 字段使用 __builtin_memcpy 而非 bpf_probe_read，因为 tracepoint 上下文
//   中的数据在 BPF 程序执行期间保证有效
// ============================================================================
SEC("tracepoint/block/block_rq_complete")
int trace_block_rq_complete(struct trace_event_raw_block_rq_completion *ctx) {
    CHECK_GATE();
    __u64 key = ctx->sector;
    struct ns_request_start *start = bpf_map_lookup_elem(&req_starts, &key);
    if (!start) return 0;  // 找不到开始记录（可能来自非 block_rq_issue 路径）

    // 预留 ring buffer 空间
    INC_STAT(STAT_TOTAL_EVENTS);
    struct il_bio_event *e = bpf_ringbuf_reserve(&bio_events, sizeof(*e), 0);
    if (!e) {
        // ring buffer 满：静默丢弃事件，但必须清理 map 记录防止泄漏
        INC_STAT(STAT_BUFFER_FULL);
        bpf_map_delete_elem(&req_starts, &key);
        return 0;
    }

    // 计算 IO 延迟
    __u64 now = bpf_ktime_get_ns();
    e->timestamp_ns = now;
    e->pid = start->pid;
    e->tid = start->tid;
    e->latency_ns = now - start->start_ns;  // 核心指标：端到端 IO 延迟

    // IO 请求参数
    e->sector = ctx->sector;       // 起始扇区号
    e->nr_sector = ctx->nr_sector; // 扇区数（每个扇区通常 512 字节）
    e->dev = ctx->dev;             // 块设备号

    // 拷贝进程名（__builtin_memcpy 是 BPF 编译器内置的安全 memcpy）
    __builtin_memcpy(&e->comm, &start->comm, TASK_COMM_LEN);

    // 读写类型判断：rwbs[0] == 'W' → 写操作(1)，否则 → 读操作(0)
    // rwbs 是字符数组: "R"=读, "W"=写, "D"=丢弃, "F"=flush 等
    e->rwflag = (ctx->rwbs[0] == 'W') ? 1 : 0;

    // 提交事件并清理追踪记录
    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&req_starts, &key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
