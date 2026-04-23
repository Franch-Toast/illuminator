#include "../include/common.bpf.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} cpu_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(__u64));
    __uint(max_entries, 16384);
} stacks SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct il_stack_key);
    __type(value, __u64);
} stack_counts SEC(".maps");

SEC("perf_event")
int on_cpu_sample(struct bpf_perf_event_data *ctx) {
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = pid_tgid >> 32;
    __u32 tid = (__u32)pid_tgid;

    if (pid == 0) return 0;

    struct il_cpu_sample_event *e;
    e = bpf_ringbuf_reserve(&cpu_events, sizeof(*e), 0);
    if (!e) return 0;

    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid;
    e->tid = tid;
    e->cpu = bpf_get_smp_processor_id();
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    e->kernel_stack_id = bpf_get_stackid(ctx, &stacks,
                                          BPF_F_FAST_STACK_CMP);
    e->user_stack_id = bpf_get_stackid(ctx, &stacks,
                                        BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

    // Also increment aggregated count
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

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
