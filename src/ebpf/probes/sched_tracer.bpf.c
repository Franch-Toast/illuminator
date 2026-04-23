#include "../include/common.bpf.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} sched_events SEC(".maps");

// Track when a task was enqueued to measure runqueue latency
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);  // pid
    __type(value, __u64);  // enqueue timestamp
} wakeup_ts SEC(".maps");

SEC("tracepoint/sched/sched_wakeup")
int trace_sched_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    __u32 pid = ctx->pid;
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);

    struct il_sched_event *e = bpf_ringbuf_reserve(&sched_events, sizeof(*e), 0);
    if (!e) return 0;

    e->timestamp_ns = ts;
    e->next_pid = pid;
    e->next_tid = pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 1;  // wakeup
    e->latency_ns = 0;

    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->comm);

    __u64 cur_pid_tgid = bpf_get_current_pid_tgid();
    e->prev_pid = cur_pid_tgid >> 32;
    e->prev_tid = (__u32)cur_pid_tgid;
    bpf_get_current_comm(&e->prev_comm, sizeof(e->prev_comm));

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tracepoint/sched/sched_switch")
int trace_sched_switch(struct trace_event_raw_sched_switch *ctx) {
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

    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm), ctx->prev_comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->next_comm);

    // Calculate runqueue latency
    __u32 next_pid = ctx->next_pid;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
    if (enqueue_ts) {
        e->latency_ns = ts - *enqueue_ts;
        bpf_map_delete_elem(&wakeup_ts, &next_pid);
    } else {
        e->latency_ns = 0;
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
