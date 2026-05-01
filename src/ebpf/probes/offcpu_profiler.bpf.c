#include "../include/common.bpf.h"

struct offcpu_key {
    __u32 pid;
    __u32 tid;
};

struct offcpu_val {
    __u64 timestamp_ns;
    __s32 kernel_stack_id;
    __s32 user_stack_id;
    char comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} offcpu_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));
    __uint(max_entries, 16384);
} offcpu_stacks SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_val);
} offcpu_start SEC(".maps");

// Minimum off-CPU duration to record (in nanoseconds), set from userspace
const volatile __u64 min_duration_ns = 100000; // 100us default

SEC("tracepoint/sched/sched_switch")
int trace_offcpu(struct trace_event_raw_sched_switch *ctx) {
    __u64 ts = bpf_ktime_get_ns();
    __u32 cpu = bpf_get_smp_processor_id();

    // Handle prev task going off-CPU: record start time and stack
    __u32 prev_pid = ctx->prev_pid;
    if (prev_pid != 0) {
        struct offcpu_key key = {.pid = prev_pid, .tid = prev_pid};
        struct offcpu_val val = {};
        val.timestamp_ns = ts;
        val.kernel_stack_id =
            bpf_get_stackid(ctx, &offcpu_stacks, BPF_F_FAST_STACK_CMP);
        val.user_stack_id = bpf_get_stackid(
            ctx, &offcpu_stacks, BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);
        bpf_probe_read_kernel_str(&val.comm, sizeof(val.comm), ctx->prev_comm);
        bpf_map_update_elem(&offcpu_start, &key, &val, BPF_ANY);
    }

    // Handle next task coming on-CPU: check if we recorded its off-CPU start
    __u32 next_pid = ctx->next_pid;
    if (next_pid == 0)
        return 0;

    struct offcpu_key next_key = {.pid = next_pid, .tid = next_pid};
    struct offcpu_val *start = bpf_map_lookup_elem(&offcpu_start, &next_key);
    if (!start)
        return 0;

    __u64 duration = ts - start->timestamp_ns;
    if (duration < min_duration_ns) {
        bpf_map_delete_elem(&offcpu_start, &next_key);
        return 0;
    }

    struct il_offcpu_event *e =
        bpf_ringbuf_reserve(&offcpu_events, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&offcpu_start, &next_key);
        return 0;
    }

    e->timestamp_ns = start->timestamp_ns;
    e->pid = next_pid;
    e->tid = next_pid;
    e->cpu = cpu;
    e->duration_ns = duration;
    e->kernel_stack_id = start->kernel_stack_id;
    e->user_stack_id = start->user_stack_id;
    __builtin_memcpy(e->comm, start->comm, TASK_COMM_LEN);

    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&offcpu_start, &next_key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
