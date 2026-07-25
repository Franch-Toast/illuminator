#include "../include/common.bpf.h"

// ============================================================================
// 文件：illuminator/src/ebpf_common/probes/cpu_sampler.bpf.c
// ============================================================================
//
// 【作用】
// CPU 采样器 eBPF 探针。通过 perf_event 子系统捕获每个 CPU 核心上
// 当前运行的进程及其完整调用栈，并实时推送到 ring buffer 供用户态消费。
//
// 【工作原理】
// 1. 与 cpu_profiler 的对比：
//    - cpu_sampler 是"流式优先"模式：每个 perf_event 触发都立即构造
//      il_cpu_sample_event 并通过 ring buffer 推送到用户态。
//    - cpu_profiler 是"聚合优先"模式：默认只更新 stack_counts map。
//    - cpu_sampler 适用于即时诊断和实时火焰图场景。
//
// 2. 触发机制：
//    用户态通过 perf_event_open() 创建 CPU 时钟定时器，eBPF 程序
//    on_cpu_sample 被注册为该事件的回调函数。采样频率由用户态配置
//    （如 99Hz），每秒约 99 次 × CPU 核心数 次触发。
//
// 3. 数据流：
//    perf_event 触发 → on_cpu_sample() →
//    ┌─ bpf_get_current_pid_tgid() 获取进程信息
//    ├─ bpf_get_stackid() 获取内核/用户态调用栈
//    ├─ stack_counts map 原子加（背景聚合）
//    └─ bpf_ringbuf_reserve/submit 推送实时事件
//
// 4. Ring buffer 溢出处理：
//    当 ring buffer 满时，bpf_ringbuf_reserve() 返回 NULL，
//    本次采样静默丢弃。用户态应通过监控 ring buffer 获取丢包率
//    来确定是否需要增大 IL_RINGBUF_SIZE 或降低采样频率。

// ============================================================================
// BPF Maps 定义
// ============================================================================

// cpu_events：CPU 采样事件 ring buffer
// 实时推送通道，每个采样事件 48 字节，256KB 可缓冲约 5400 个事件
// 对于 8 核 99Hz 采样（≈800 events/s），可承受约 6.7 秒的消费延迟
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} cpu_events SEC(".maps");

// stacks：调用栈存储（去重）
// STACK_TRACE 类型 map 自动对相同栈帧序列去重
// 每个 stack_id 映射到一组栈帧地址，避免重复传输大量栈数据
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    // 每栈最多 MAX_STACK_DEPTH(127) 帧 × 8字节 = 1016 字节
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));
    __uint(max_entries, 16384);
} stacks SEC(".maps");

// stack_counts：调用栈聚合计数器
// 用于背景累加统计，即使用户态不消费实时事件，也能事后查询累计分布
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct il_stack_key);
    __type(value, __u64);
} stack_counts SEC(".maps");

DECLARE_META_STATS();

// ============================================================================
// on_cpu_sample：CPU 采样处理函数（perf_event 类型）
//
// 功能：捕获当前 CPU 核心上的运行进程信息、获取调用栈快照，并实时推送事件。
//
// 参数：ctx - perf_event 上下文
// 返回：0
//
// 处理流程：
// 1. 提取进程/线程 ID
// 2. 从 ring buffer 预留事件空间
// 3. 填充事件字段（时间戳、进程信息、调用栈等）
// 4. 更新聚合计数器（背景统计）
// 5. 提交事件到 ring buffer
// ============================================================================
SEC("perf_event")
int on_cpu_sample(struct bpf_perf_event_data *ctx) {
    // ---------------------------------------------------------------
    // 步骤 1：获取当前进程标识
    // pid_tgid 高 32 位是 tgid（进程 ID），低 32 位是 pid（线程 ID）
    // 跳过 pid=0 的空闲任务（swapper），它没有有意义的用户态上下文
    // ---------------------------------------------------------------
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = pid_tgid >> 32;
    __u32 tid = (__u32)pid_tgid;

    if (pid == 0) return 0;

    // ---------------------------------------------------------------
    // 步骤 2：从 ring buffer 预留事件空间
    // sizeof(il_cpu_sample_event) ≈ 48 字节
    // 预留失败（返回 NULL）表示 ring buffer 已满，本次采样静默丢弃
    // ---------------------------------------------------------------
    INC_STAT(STAT_TOTAL_EVENTS);
    struct il_cpu_sample_event *e;
    e = bpf_ringbuf_reserve(&cpu_events, sizeof(*e), 0);
    if (!e) {
        INC_STAT(STAT_BUFFER_FULL);
        return 0;
    }

    // ---------------------------------------------------------------
    // 步骤 3：填充事件字段
    // timestamp_ns = bpf_ktime_get_ns()  获取单调递增纳秒时间戳
    // cpu = bpf_get_smp_processor_id()   当前执行所在的 CPU 编号
    // ---------------------------------------------------------------
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid;
    e->tid = tid;
    e->cpu = bpf_get_smp_processor_id();
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // ---------------------------------------------------------------
    // 步骤 4：获取内核态和用户态调用栈
    // kernel_stack_id：内核栈的 stack id（通过 bpf_get_stackid 从 ctx 获取）
    // user_stack_id：用户栈的 stack id（加 BPF_F_USER_STACK 标志）
    // BPF_F_FAST_STACK_CMP：优化标志，跳过帧指针验证以提升性能
    // ---------------------------------------------------------------
    e->kernel_stack_id = bpf_get_stackid(ctx, &stacks,
                                          BPF_F_FAST_STACK_CMP);
    e->user_stack_id = bpf_get_stackid(ctx, &stacks,
                                        BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

    // ---------------------------------------------------------------
    // 步骤 5：更新聚合计数器（背景统计）
    // 构造与 stack_counts map 匹配的 key 结构
    // 原子递增：支持多 CPU 并发写入同一 key
    // BPF_NOEXIST：仅在 key 不存在时插入，避免竞态覆盖
    // ---------------------------------------------------------------
    struct il_stack_key key = {};
    key.pid = pid;
    key.tid = tid;
    key.kernel_stack_id = e->kernel_stack_id;
    key.user_stack_id = e->user_stack_id;
    bpf_get_current_comm(&key.comm, sizeof(key.comm));

    __u64 *count = bpf_map_lookup_elem(&stack_counts, &key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 init = 1;
        bpf_map_update_elem(&stack_counts, &key, &init, BPF_NOEXIST);
    }

    // ---------------------------------------------------------------
    // 步骤 6：提交事件到 ring buffer，使其对用户态可见
    // 提交后，用户态的 ring_buffer__poll/ring_buffer__consume 可读取此事件
    // ---------------------------------------------------------------
    bpf_ringbuf_submit(e, 0);
    return 0;
}

// BPF 许可证声明：GPL 兼容的 helper 函数需要此声明
char LICENSE[] SEC("license") = "GPL";
