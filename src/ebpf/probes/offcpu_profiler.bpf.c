#include "../include/common.bpf.h"

// ============================================================================
// 文件：illuminator/src/ebpf/probes/offcpu_profiler.bpf.c
// ============================================================================
//
// 【作用】
// Off-CPU（离CPU）剖析 eBPF 探针。追踪任务离开 CPU 后的阻塞时长和
// 阻塞原因（在哪段代码中进入了阻塞状态）。补充 CPU profiling
// 的"on-CPU"视角，形成全链路性能分析闭环。
//
// 【工作原理】
// 1. 核心思路：sched_switch tracepoint 每次被调用时（即每次上下文切换）
//    同时处理"切出"和"切入"两个方向的逻辑：
//
//    - 切出方向（prev task going off-CPU）：
//      记录切出任务的身份（pid/tid）、离开时的时间戳、以及离 CPU 前
//      的调用栈快照（记录"阻塞在哪里发生的"）。存入 offcpu_start map。
//
//    - 切入方向（next task coming on-CPU）：
//      检查切入的任务之前是否有 off-CPU 开始记录。
//      如果有：计算阻塞时长 = now - start.timestamp_ns。
//      如果时长 ≥ min_duration_ns（默认 100μs）：构造 il_offcpu_event 推送。
//
// 2. 调用栈记录策略：
//    在任务切出 CPU 的时刻抓取调用栈（通过 bpf_get_stackid）。
//    此时栈中可能包含：
//    - 阻塞等待的内核函数（如 futex_wait, poll_schedule_timeout）
//    - 触发阻塞的系统调用入口
//    - 用户态发起阻塞操作的调用链
//    这样火焰图可以展示"阻塞发生的位置"而不是"占用 CPU 的位置"。
//
// 3. 最小值过滤：
//    min_duration_ns 默认 100 微秒（100000ns），过滤掉极短暂的调度
//    （如时间片到期导致的正常切换），只关注有意义的阻塞等待。
//    用户态可通过此 volatile 变量动态调整阈值。
//
// 4. 与 sched_analyzer/sched_tracer 的区别：
//    - offcpu_profiler：关注阻塞的原因和时长（调用栈视角）
//    - sched_analyzer：关注调度行为的统计指标（聚合统计视角）
//    - sched_tracer：关注每次上下文切换的详细日志（事件日志视角）

// ============================================================================
// 本地数据结构（BPF maps 内部存储用）
// ============================================================================

// offcpu_key：off-CPU 追踪记录的键
// pid+tid 组合唯一标识一个任务
struct offcpu_key {
    __u32 pid;  // 进程 ID（tgid）
    __u32 tid;  // 线程 ID（pid）
};

// offcpu_val：off-CPU 追踪记录的值
// 记录任务离开 CPU 时的快照信息：时间、调用栈、任务名
struct offcpu_val {
    __u64 timestamp_ns;          // 离开 CPU 的时间戳
    __s32 kernel_stack_id;       // 内核调用栈 ID（阻塞发生时的内核栈）
    __s32 user_stack_id;         // 用户态调用栈 ID（阻塞发生时的用户栈）
    char comm[TASK_COMM_LEN];    // 任务名称
};

// ============================================================================
// BPF Maps 定义
// ============================================================================

// offcpu_events：Off-CPU 事件 ring buffer
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} offcpu_events SEC(".maps");

// offcpu_stacks：阻塞时的调用栈存储（去重）
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));
    __uint(max_entries, 16384);
} offcpu_stacks SEC(".maps");

// offcpu_start：未完成的 off-CPU 等待记录
// key=(pid, tid)，value=离开 CPU 时的快照
// 当任务重新回到 CPU 时，按 key 查找并计算时长
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);  // 支持 65536 个任务同时处于阻塞状态
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_val);
} offcpu_start SEC(".maps");

// ============================================================================
// min_duration_ns：最小 Off-CPU 时长阈值（volatile：用户态可动态配置）
//
// 只有阻塞时长 ≥ 此值的 off-CPU 事件才会被记录和推送。
// 默认 100000ns (100μs)，过滤掉时间片到期的正常调度。
//
// volatile 关键字告知 BPF 编译器此变量可被用户态通过 map 修改。
// 对于全局 volatile 变量，BPF 编译器不会常量折叠优化，确保动态性。
// ============================================================================
const volatile __u64 min_duration_ns = 100000; // 100μs 默认值

// ============================================================================
// trace_offcpu：调度切换 tracepoint 处理函数
//
// 功能：同时处理切出（记录 off-CPU 开始）和切入（计算 off-CPU 时长）两个逻辑。
//
// 参数：ctx - sched_switch tracepoint 上下文
//       ctx->prev_comm/prev_pid = 切出的任务信息
//       ctx->next_comm/next_pid = 切入的任务信息
//
// 处理流程：
// 1. 【切出】如果 prev_pid ≠ 0（非空闲任务），记录离开 CPU 的快照
// 2. 【切入】如果 next_pid ≠ 0，查找该任务之前的 off-CPU 开始记录
// 3. 计算时长 duration = now - start.timestamp_ns
// 4. 如果 duration ≥ min_duration_ns，构造事件并推送
// 5. 清理 offcpu_start 中的已完成记录
//
// 注意：对于 tid 的处理采用 pid==tid（即 tgid==pid），简化设计。
//       对于多线程进程，可通过将 key 改为 (tgid, pid) 来区分。
// ============================================================================
SEC("tracepoint/sched/sched_switch")
int trace_offcpu(struct trace_event_raw_sched_switch *ctx) {
    __u64 ts = bpf_ktime_get_ns();          // 当前时间
    __u32 cpu = bpf_get_smp_processor_id(); // 当前 CPU 编号

    // ============================================================
    // 阶段 1：处理切出的任务（记录 off-CPU 开始快照）
    // ============================================================
    __u32 prev_pid = ctx->prev_pid;
    if (prev_pid != 0) {
        struct offcpu_key key = {.pid = prev_pid, .tid = prev_pid};
        struct offcpu_val val = {};

        // 记录离开 CPU 的时间戳
        val.timestamp_ns = ts;

        // 在切出的瞬间抓取内核调用栈（定位"谁导致了阻塞"）
        val.kernel_stack_id =
            bpf_get_stackid(ctx, &offcpu_stacks, BPF_F_FAST_STACK_CMP);

        // 同时抓取用户态调用栈（定位业务代码中导致阻塞的位置）
        val.user_stack_id = bpf_get_stackid(
            ctx, &offcpu_stacks, BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

        // 安全读取任务名（通过 bpf_probe_read_kernel_str 从内核内存复制）
        bpf_probe_read_kernel_str(&val.comm, sizeof(val.comm), ctx->prev_comm);

        // 存入等待表，BPF_ANY 允许覆盖（如果之前的记录尚未被消费）
        bpf_map_update_elem(&offcpu_start, &key, &val, BPF_ANY);
    }

    // ============================================================
    // 阶段 2：处理切入的任务（检查并计算 off-CPU 时长）
    // ============================================================
    __u32 next_pid = ctx->next_pid;
    // 跳过空闲任务（没有意义去追踪 idle task 的 off-CPU）
    if (next_pid == 0)
        return 0;

    // 在 offcpu_start 中查找该任务之前的离开记录
    struct offcpu_key next_key = {.pid = next_pid, .tid = next_pid};
    struct offcpu_val *start = bpf_map_lookup_elem(&offcpu_start, &next_key);
    if (!start)
        return 0;  // 没有之前的记录（可能是首次被调度）

    // 计算 off-CPU 时长
    __u64 duration = ts - start->timestamp_ns;

    // 应用最小值过滤：太短的时间片切换不关注
    if (duration < min_duration_ns) {
        bpf_map_delete_elem(&offcpu_start, &next_key);
        return 0;
    }

    // -----------------------------------------------------------
    // 构造 off-CPU 事件并推送
    // -----------------------------------------------------------
    struct il_offcpu_event *e =
        bpf_ringbuf_reserve(&offcpu_events, sizeof(*e), 0);
    if (!e) {
        // ring buffer 满：丢弃事件，但必须清理 map 以防泄漏
        bpf_map_delete_elem(&offcpu_start, &next_key);
        return 0;
    }

    e->timestamp_ns = start->timestamp_ns;  // 阻塞开始时间
    e->pid = next_pid;
    e->tid = next_pid;
    e->cpu = cpu;
    e->duration_ns = duration;              // 阻塞持续时长（核心指标）
    e->kernel_stack_id = start->kernel_stack_id;
    e->user_stack_id = start->user_stack_id;
    __builtin_memcpy(e->comm, start->comm, TASK_COMM_LEN);

    // 提交事件并清理追踪记录
    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&offcpu_start, &next_key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
