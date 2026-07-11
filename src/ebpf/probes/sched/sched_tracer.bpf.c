// ============================================================================
// sched_tracer.bpf.c — eBPF 调度器事件追踪 BPF 探针
// ============================================================================
//
// 本 BPF 程序挂载在内核调度器的两个关键 tracepoint 上：
//   1. sched_wakeup:  进程被唤醒时触发
//   2. sched_switch:  上下文切换时触发
//
// 核心功能：追踪调度器事件并计算运行队列延迟
// ============================================================================
//
// Ring Buffer: sched_events（256KB） — 事件推送通道
// Hash Map:    wakeup_ts — 记录进程被唤醒的时间戳
//
// ============================================================================
// sched_wakeup tracepoint 探针
// ============================================================================
// 在进程被唤醒时触发：
//   1. 记录当前时间戳到 wakeup_ts map（按 PID 为键）
//   2. 构造调度事件 il_sched_event（event_type=1 表示 wakeup）
//   3. 填充事件详情（next 为目标进程，prev 为当前触发者）
//   4. 推送到 sched_events Ring Buffer
//
// ============================================================================
// sched_switch tracepoint 探针
// ============================================================================
// 在上下文切换时触发：
//   1. 构造调度事件 il_sched_event（event_type=0 表示 switch）
//   2. 查找被切换进的进程的 wakeup 时间戳
//   3. 计算运行队列延迟 = 当前时间 - wakeup 时间
//   4. 推送到 sched_events Ring Buffer
// ============================================================================

#include "../include/common.bpf.h"

// 初始化 Ring Buffer 用于向用户态推送调度事件
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} sched_events SEC(".maps");

// 记录进程被唤醒时的时间戳（用于计算运行队列延迟）
// key: pid, value: 唤醒时的时间戳(纳秒)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, __u64);
} wakeup_ts SEC(".maps");

DECLARE_COLLECTION_GATE();

// ---- sched_wakeup tracepoint ----
SEC("tracepoint/sched/sched_wakeup")
int trace_sched_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    __u32 pid = ctx->pid;
    __u64 ts = bpf_ktime_get_ns();

    bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);

    CHECK_GATE();

    struct il_sched_event *e = bpf_ringbuf_reserve(&sched_events, sizeof(*e), 0);
    if (!e) return 0;

    // 填充事件信息
    e->timestamp_ns = ts;
    e->next_pid = pid;
    e->next_tid = pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 1;  // wakeup
    e->latency_ns = 0;

    // 安全读取内核中的进程名（comm 字段）
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->comm);

    // 获取当前触发这个 tracepoint 的进程信息
    __u64 cur_pid_tgid = bpf_get_current_pid_tgid();
    e->prev_pid = cur_pid_tgid >> 32;
    e->prev_tid = (__u32)cur_pid_tgid;
    bpf_get_current_comm(&e->prev_comm, sizeof(e->prev_comm));

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// ---- sched_switch tracepoint ----
SEC("tracepoint/sched/sched_switch")
int trace_sched_switch(struct trace_event_raw_sched_switch *ctx) {
    CHECK_GATE();
    struct il_sched_event *e = bpf_ringbuf_reserve(&sched_events, sizeof(*e), 0);
    if (!e) return 0;

    __u64 ts = bpf_ktime_get_ns();
    e->timestamp_ns = ts;
    e->prev_pid = ctx->prev_pid;
    e->next_pid = ctx->next_pid;
    e->prev_tid = ctx->prev_pid;
    e->next_tid = ctx->next_pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 0;  // switch

    // 安全读取内核中的进程名
    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm), ctx->prev_comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->next_comm);

    // 计算运行队列延迟：被切进的进程从被唤醒到真正上 CPU 的等待时间
    __u32 next_pid = ctx->next_pid;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
    if (enqueue_ts) {
        e->latency_ns = ts - *enqueue_ts;   // 当前时间 - 入队时间
        bpf_map_delete_elem(&wakeup_ts, &next_pid);  // 清理已处理的时间戳
    } else {
        e->latency_ns = 0;  // 未找到唤醒记录（如首次调度），延迟为 0
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
