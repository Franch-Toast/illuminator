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

// offcpu_key：off-CPU 追踪记录的键（仅用 tid，系统唯一）
struct offcpu_key {
    __u32 tid;  // 线程 ID（内核 task->pid，系统范围内唯一）
};

// offcpu_val：off-CPU 追踪记录的值
struct offcpu_val {
    __u64 timestamp_ns;          // 离开 CPU 的时间戳
    __u32 tgid;                  // 进程组 ID（用户态 PID）
    __s32 kernel_stack_id;       // 内核调用栈 ID
    __s32 user_stack_id;         // 用户态调用栈 ID
    char comm[TASK_COMM_LEN];    // 线程名称
};

// ============================================================================
// BPF Maps 定义
// ============================================================================

// offcpu_events：Off-CPU 事件 ring buffer（低频信号通道）
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} offcpu_events SEC(".maps");

// offcpu_stats：内核侧聚合 map（按 tgid+tid+stack 聚合 off-CPU 时长和计数）
// 包含 tid 以区分同进程内不同线程的 off-CPU 行为
struct offcpu_stat_key {
    __u32 tgid;             // 进程组 ID
    __u32 tid;              // 线程 ID
    __s32 kernel_stack_id;
    __s32 user_stack_id;
};

struct offcpu_stat_val {
    __u64 total_ns;         // 累计 off-CPU 时长
    __u32 count;            // 事件计数
    __u32 cpu;              // 最近一次 CPU
    char comm[TASK_COMM_LEN];  // 线程名
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, struct offcpu_stat_key);
    __type(value, struct offcpu_stat_val);
} offcpu_stats SEC(".maps");

// offcpu_signal_ts：上次向用户态发送聚合信号的时间戳
// 用于限流：最多 1 次/秒通知用户态
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} offcpu_signal_ts SEC(".maps");

// offcpu_stacks：阻塞时的调用栈存储（去重）
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));
    __uint(max_entries, 16384);
} offcpu_stacks SEC(".maps");

// offcpu_start：未完成的 off-CPU 等待记录
// key=tid（系统唯一），value=离开 CPU 时的快照（含 tgid）
// 当任务重新回到 CPU 时，按 tid 查找并计算时长
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_val);
} offcpu_start SEC(".maps");

// ============================================================================
// offcpu_cfg：运行时配置（用户态通过 bpf_map_update_elem 写入）
//   index 0: flags
//     bit0 = user_stacks 启用
//     bit1 = kernel_stacks 启用
//     bit2 = PID 白名单过滤启用
//     bit3 = 进程名白名单过滤启用
//     bit4 = 全局启用标志（0=禁用所有采集，用于启动延迟期）
//   index 1: min_duration_ns 低 32 位
//   index 2: min_duration_ns 高 32 位
// ============================================================================
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 3);
    __type(key, __u32);
    __type(value, __u32);
} offcpu_cfg SEC(".maps");

// offcpu_target_pids：PID 白名单 hash map（仅 cfg bit2 时生效）
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u8);
} offcpu_target_pids SEC(".maps");

// offcpu_target_comms：进程名白名单 hash map（仅 cfg bit3 时生效）
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, char[TASK_COMM_LEN]);
    __type(value, __u8);
} offcpu_target_comms SEC(".maps");

// offcpu_pidns_cfg：PID Namespace 配置（用于 bpf_get_ns_current_pid_tgid）
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct il_pidns_config);
} offcpu_pidns_cfg SEC(".maps");

static __always_inline __u32 offcpu_cfg_flags(void) {
    __u32 k = 0;
    __u32 *p = bpf_map_lookup_elem(&offcpu_cfg, &k);
    if (p) return *p;
    return 0;  // 默认: 全部禁用（需要用户态显式启用 bit4）
}

// 从 offcpu_cfg map（index 1/2）读取用户态配置的阈值，避免编译时固定
static __always_inline __u64 offcpu_get_min_duration(void) {
    __u32 k1 = 1, k2 = 2;
    __u32 *lo = bpf_map_lookup_elem(&offcpu_cfg, &k1);
    __u32 *hi = bpf_map_lookup_elem(&offcpu_cfg, &k2);
    if (lo && hi)
        return ((__u64)*hi << 32) | (__u64)*lo;
    return 10000000;  // 默认 10ms
}

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
    __u32 flags = offcpu_cfg_flags();

    // 全局使能检查（bit4）：启动延迟期或未配置时，完全跳过所有逻辑
    if (!(flags & 16))
        return 0;

    __u64 ts = bpf_ktime_get_ns();
    __u32 cpu = bpf_get_smp_processor_id();

    // ============================================================
    // 阶段 1：处理切出的任务（记录 off-CPU 开始快照）
    // ============================================================
    __u32 prev_tid = ctx->prev_pid;  // tracepoint 的 prev_pid 实际是 TID
    if (prev_tid == 0)
        goto phase2;

    // 获取 tgid（进程组 ID）— sched_switch 中 current 就是 prev
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 prev_tgid = pid_tgid >> 32;

    // PID Namespace 翻译：将 root-ns tgid 转为用户态可见的 namespace-local tgid
    {
        __u32 ns_key = 0;
        struct il_pidns_config *ns_cfg = bpf_map_lookup_elem(&offcpu_pidns_cfg, &ns_key);
        if (ns_cfg && ns_cfg->ino != 0) {
            struct bpf_pidns_info ns_info = {};
            long ret = bpf_get_ns_current_pid_tgid(
                ns_cfg->dev, ns_cfg->ino, &ns_info, sizeof(ns_info));
            if (ret == 0) {
                prev_tgid = ns_info.tgid;
                prev_tid = ns_info.pid;
            }
        }
    }

    // PID 白名单过滤（flags bit2=4）：用 tgid 匹配，捕获进程的所有线程
    // 此时 prev_tgid 已经是 namespace-local PID（如果配置了 pidns）
    if (flags & 4) {
        if (!bpf_map_lookup_elem(&offcpu_target_pids, &prev_tgid))
            goto phase2;
    }

    // 进程名白名单过滤（flags bit3=8）：读取 group_leader->comm（主线程名）
    // 确保多线程程序的所有线程都能匹配（不管线程自身 comm 如何改变）
    if (flags & 8) {
        struct task_struct *task = (void *)bpf_get_current_task();
        char leader_comm[TASK_COMM_LEN];
        __builtin_memset(leader_comm, 0, sizeof(leader_comm));
        bpf_probe_read_kernel_str(&leader_comm, sizeof(leader_comm),
                                  BPF_CORE_READ(task, group_leader, comm));
        if (!bpf_map_lookup_elem(&offcpu_target_comms, &leader_comm))
            goto phase2;
    }

    {
        struct offcpu_key key = {.tid = prev_tid};
        struct offcpu_val val = {};
        val.timestamp_ns = ts;
        val.tgid = prev_tgid;

        // 内核堆栈
        if (flags & 2) {
            val.kernel_stack_id =
                bpf_get_stackid(ctx, &offcpu_stacks, BPF_F_FAST_STACK_CMP);
        } else {
            val.kernel_stack_id = -1;
        }

        // 用户态堆栈 — 跳过内核线程（PF_KTHREAD = 0x00200000）
        if (flags & 1) {
            struct task_struct *task = (void *)bpf_get_current_task();
            __u32 task_flags = BPF_CORE_READ(task, flags);
            if (task_flags & 0x00200000) {
                val.user_stack_id = -1;
            } else {
                val.user_stack_id = bpf_get_stackid(
                    ctx, &offcpu_stacks,
                    BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);
            }
        } else {
            val.user_stack_id = -1;
        }

        // 保存线程自身的 comm（用于聚合时标识具体线程）
        bpf_get_current_comm(&val.comm, sizeof(val.comm));
        bpf_map_update_elem(&offcpu_start, &key, &val, BPF_ANY);
    }

phase2:
    // ============================================================
    // 阶段 2：处理切入的任务（检查并计算 off-CPU 时长）
    // 设计参考 perf_ebpf：内核 map 聚合 + 低频信号通知用户态
    // ============================================================
    ;
    __u32 next_tid = ctx->next_pid;  // tracepoint 的 next_pid 实际是 TID
    if (next_tid == 0)
        return 0;

    struct offcpu_key next_key = {.tid = next_tid};
    struct offcpu_val *start = bpf_map_lookup_elem(&offcpu_start, &next_key);
    if (!start)
        return 0;

    __u64 duration = ts - start->timestamp_ns;

    __u64 threshold = offcpu_get_min_duration();
    if (duration < threshold) {
        bpf_map_delete_elem(&offcpu_start, &next_key);
        return 0;
    }

    // 聚合到 offcpu_stats map（按 tgid+tid+stack 聚合，区分线程）
    struct offcpu_stat_key skey = {
        .tgid = start->tgid,
        .tid = next_tid,
        .kernel_stack_id = start->kernel_stack_id,
        .user_stack_id = start->user_stack_id,
    };
    struct offcpu_stat_val *existing = bpf_map_lookup_elem(&offcpu_stats, &skey);
    if (existing) {
        __sync_fetch_and_add(&existing->total_ns, duration);
        __sync_fetch_and_add(&existing->count, 1);
        existing->cpu = cpu;
    } else {
        struct offcpu_stat_val newval = {};
        newval.total_ns = duration;
        newval.count = 1;
        newval.cpu = cpu;
        __builtin_memcpy(newval.comm, start->comm, TASK_COMM_LEN);
        bpf_map_update_elem(&offcpu_stats, &skey, &newval, BPF_NOEXIST);
    }

    // 低频信号：最多 1 次/秒通知用户态消费 offcpu_stats
    __u32 sig_k = 0;
    __u64 *last_signal = bpf_map_lookup_elem(&offcpu_signal_ts, &sig_k);
    if (!last_signal || (ts - *last_signal) >= 1000000000ULL) {
        struct il_offcpu_event *e =
            bpf_ringbuf_reserve(&offcpu_events, sizeof(*e), 0);
        if (e) {
            __builtin_memset(e, 0, sizeof(*e));
            e->timestamp_ns = ts;
            e->pid = 0;  // pid=0 标识为聚合信号
            bpf_ringbuf_submit(e, 0);
        }
        __u64 now = ts;
        bpf_map_update_elem(&offcpu_signal_ts, &sig_k, &now, BPF_ANY);
    }

    bpf_map_delete_elem(&offcpu_start, &next_key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
