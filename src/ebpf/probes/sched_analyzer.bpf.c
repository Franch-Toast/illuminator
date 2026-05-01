#include "../include/common.bpf.h"

// Bit0 = emit detailed ringbuf events, bit1 = track migrations (maps + detailed)
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} sched_analyzer_cfg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} sched_analyzer_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, struct il_sched_stats);
} sched_agg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, __u64);
} wakeup_ts SEC(".maps");

static __always_inline __u32 sched_analyzer_flags(void) {
    __u32 k = 0;
    __u32 *p = bpf_map_lookup_elem(&sched_analyzer_cfg, &k);
    if (p)
        return *p;
    return 3;
}

static __always_inline void sched_agg_add_latency(__u32 pid, __u64 latency_ns,
                                                   const char *comm) {
    struct il_sched_stats init = {};
    struct il_sched_stats *st = bpf_map_lookup_elem(&sched_agg, &pid);
    if (!st) {
        init.switch_count = 1;
        init.total_runqueue_latency_ns = latency_ns;
        init.max_runqueue_latency_ns = latency_ns;
        init.migrate_count = 0;
        if (comm)
            bpf_probe_read_kernel_str(&init.comm, sizeof(init.comm), comm);
        bpf_map_update_elem(&sched_agg, &pid, &init, BPF_ANY);
        return;
    }

    st->switch_count += 1;
    st->total_runqueue_latency_ns += latency_ns;
    if (latency_ns > st->max_runqueue_latency_ns)
        st->max_runqueue_latency_ns = latency_ns;
    if (comm)
        bpf_probe_read_kernel_str(&st->comm, sizeof(st->comm), comm);
}

SEC("tracepoint/sched/sched_wakeup")
int sched_analyzer_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    __u32 pid = ctx->pid;
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);

    if (!(sched_analyzer_flags() & 1))
        return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->timestamp_ns = ts;
    e->next_pid = pid;
    e->next_tid = pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 1;
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
int sched_analyzer_switch(struct trace_event_raw_sched_switch *ctx) {
    __u64 ts = bpf_ktime_get_ns();

    __u32 next_pid = ctx->next_pid;
    __u64 latency_ns = 0;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
    if (enqueue_ts) {
        latency_ns = ts - *enqueue_ts;
        bpf_map_delete_elem(&wakeup_ts, &next_pid);
    }

    if (next_pid)
        sched_agg_add_latency(next_pid, latency_ns, ctx->next_comm);

    if (!(sched_analyzer_flags() & 1))
        return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->timestamp_ns = ts;
    e->prev_pid = ctx->prev_pid;
    e->next_pid = ctx->next_pid;
    e->prev_tid = ctx->prev_pid;
    e->next_tid = ctx->next_pid;
    e->cpu = bpf_get_smp_processor_id();
    e->event_type = 0;
    e->latency_ns = latency_ns;

    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm),
                               ctx->prev_comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm),
                               ctx->next_comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tracepoint/sched/sched_migrate_task")
int sched_analyzer_migrate(struct trace_event_raw_sched_migrate_task *ctx) {
    if (!(sched_analyzer_flags() & 2))
        return 0;

    __u32 pid = ctx->pid;
    struct il_sched_stats *st = bpf_map_lookup_elem(&sched_agg, &pid);
    if (st) {
        st->migrate_count += 1;
    } else {
        struct il_sched_stats init = {};
        init.migrate_count = 1;
        bpf_map_update_elem(&sched_agg, &pid, &init, BPF_ANY);
    }

    if (!(sched_analyzer_flags() & 1))
        return 0;

    struct il_sched_event *e =
        bpf_ringbuf_reserve(&sched_analyzer_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->timestamp_ns = bpf_ktime_get_ns();
    e->prev_pid = pid;
    e->next_pid = pid;
    e->prev_tid = pid;
    e->next_tid = pid;
    e->cpu = ctx->dest_cpu;
    e->event_type = 2;
    e->latency_ns = 0;
    bpf_probe_read_kernel_str(&e->prev_comm, sizeof(e->prev_comm), ctx->comm);
    bpf_probe_read_kernel_str(&e->next_comm, sizeof(e->next_comm), ctx->comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
