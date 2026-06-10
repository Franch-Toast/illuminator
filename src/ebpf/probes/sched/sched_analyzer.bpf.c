// ============================================================================
// sched_analyzer.bpf.c — eBPF 调度器分析 BPF 探针
// ============================================================================
//
// 本 BPF 程序是 sched_tracer.bpf.c 的增强版，除了追踪调度事件外，
// 还增加了：
//   1. 按进程聚合的调度统计（sched_agg Hash Map）
//   2. 可配置的双模式（详细事件流 或 纯聚合模式）
//   3. CPU 迁移追踪
//
// 三个 tracepoint 探针：
// ======================
// sched_analyzer_wakeup  — 记录唤醒时间戳 + 可选输出详细唤醒事件
// sched_analyzer_switch  — 计算运行队列延迟 + 写入聚合统计 + 可选输出切换事件
// sched_analyzer_migrate — 追踪进程跨 CPU 核迁移 + 写入聚合统计
//
// 配置位标志（通过 sched_analyzer_cfg 从用户态下发）：
// =======================================================
// bit0 = 1: 输出详细 Ring Buffer 事件
// bit1 = 1: 追踪 CPU 迁移（同时写入聚合统计）
// ============================================================================

#include "../include/common.bpf.h"

// ---- BPF Map 定义 ----

// 配置 Map：用户态下发位标志控制模式
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} sched_analyzer_cfg SEC(".maps");

// 详细事件 Ring Buffer（仅 bit0=1 时启用）
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} sched_analyzer_events SEC(".maps");

// 按进程聚合的调度统计（key=pid, value=il_sched_stats）
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, struct il_sched_stats);
} sched_agg SEC(".maps");

// 进程唤醒时间戳（key=pid, value=纳秒时间戳）
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, __u64);
} wakeup_ts SEC(".maps");

// ---- 辅助函数 ----

// 读取用户态配置的位标志（默认值为 3 = 详细事件 + 迁移追踪）
static __always_inline __u32 sched_analyzer_flags(void) {
    __u32 k = 0;
    __u32 *p = bpf_map_lookup_elem(&sched_analyzer_cfg, &k);
    if (p) return *p;
    return 3;  // 默认：详细事件 + 迁移追踪
}

// 向聚合统计写入运行队列延迟
// 首次看到某进程时初始化条目，后续则累加计数和延迟
static __always_inline void sched_agg_add_latency(__u32 pid, __u64 latency_ns,
                                                   const char *comm) {
    struct il_sched_stats init = {};
    struct il_sched_stats *st = bpf_map_lookup_elem(&sched_agg, &pid);
    if (!st) {
        // 首次：初始化新条目
        init.switch_count = 1;
        init.total_runqueue_latency_ns = latency_ns;
        init.max_runqueue_latency_ns = latency_ns;
        init.migrate_count = 0;
        if (comm)
            bpf_probe_read_kernel_str(&init.comm, sizeof(init.comm), comm);
        bpf_map_update_elem(&sched_agg, &pid, &init, BPF_ANY);
        return;
    }

    // 已有条目：累计更新
    st->switch_count += 1;
    st->total_runqueue_latency_ns += latency_ns;
    if (latency_ns > st->max_runqueue_latency_ns)
        st->max_runqueue_latency_ns = latency_ns;
    if (comm)
        bpf_probe_read_kernel_str(&st->comm, sizeof(st->comm), comm);
}

// ---- sched_wakeup tracepoint ----
SEC("tracepoint/sched/sched_wakeup")
int sched_analyzer_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    __u32 pid = ctx->pid;
    __u64 ts = bpf_ktime_get_ns();

    // 始终记录唤醒时间戳（用于延迟计算）
    bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);

    // 仅当 bit0=1 时输出详细事件
    if (!(sched_analyzer_flags() & 1)) return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e) return 0;

    e->timestamp_ns = ts;
    e->next_pid = pid;
    e->next_tid = pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 1;   // wakeup
    e->latency_ns = 0;

    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->comm);

    __u64 cur_pid_tgid = bpf_get_current_pid_tgid();
    e->prev_pid = cur_pid_tgid >> 32;
    e->prev_tid = (__u32)cur_pid_tgid;
    bpf_get_current_comm(&e->prev_comm, sizeof(e->prev_comm));

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// ---- sched_switch tracepoint ----
SEC("tracepoint/sched/sched_switch")
int sched_analyzer_switch(struct trace_event_raw_sched_switch *ctx) {
    __u64 ts = bpf_ktime_get_ns();

    // 计算运行队列延迟
    __u32 next_pid = ctx->next_pid;
    __u64 latency_ns = 0;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
    if (enqueue_ts) {
        latency_ns = ts - *enqueue_ts;  // 当前时间 - 入队时间
        bpf_map_delete_elem(&wakeup_ts, &next_pid);  // 清理
    }

    // 始终写入聚合统计（无论详细模式开关）
    if (next_pid)
        sched_agg_add_latency(next_pid, latency_ns, ctx->next_comm);

    // 仅当 bit0=1 时输出详细事件
    if (!(sched_analyzer_flags() & 1)) return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e) return 0;

    e->timestamp_ns = ts;
    e->prev_pid = ctx->prev_pid;
    e->next_pid = ctx->next_pid;
    e->prev_tid = ctx->prev_pid;
    e->next_tid = ctx->next_pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 0;        // switch
    e->latency_ns = latency_ns;

    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm), ctx->prev_comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->next_comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// ---- sched_migrate_task tracepoint ----
// 进程跨 CPU 核迁移时触发
SEC("tracepoint/sched/sched_migrate_task")
int sched_analyzer_migrate(struct trace_event_raw_sched_migrate_task *ctx) {
    // 仅当 bit1=1 时追踪迁移
    if (!(sched_analyzer_flags() & 2)) return 0;

    __u32 pid = ctx->pid;
    struct il_sched_stats *st = bpf_map_lookup_elem(&sched_agg, &pid);
    if (st) {
        st->migrate_count += 1;  // 累加迁移次数
    } else {
        // 首次：创建仅含迁移计数的条目
        struct il_sched_stats init = {};
        init.migrate_count = 1;
        bpf_map_update_elem(&sched_agg, &pid, &init, BPF_ANY);
    }

    // 仅当 bit0=1 时同时输出详细迁移事件
    if (!(sched_analyzer_flags() & 1)) return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e) return 0;

    e->timestamp_ns = bpf_ktime_get_ns();
    e->prev_pid = pid;
    e->next_pid = pid;
    e->prev_tid = pid;
    e->next_tid = pid;
    e->cpu = ctx->dest_cpu;    // 目标 CPU 核
    e->event_type = 2;         // migrate
    e->latency_ns = 0;

    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm), ctx->comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
