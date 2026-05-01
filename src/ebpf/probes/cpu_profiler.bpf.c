#include "../include/common.bpf.h"

// Bit flags (userspace): bit0 = emit ringbuf stream events, bit1 = pid allow-list,
// bit2 = comm allow-list
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} cpu_profiler_cfg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u8);
} target_pids SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, char[TASK_COMM_LEN]);
    __type(value, __u8);
} target_comms SEC(".maps");

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

    if (pid == 0)
        return 0;

    __u32 cfg_k = 0;
    __u32 cfg_flags = 1;
    __u32 *cfg_p = bpf_map_lookup_elem(&cpu_profiler_cfg, &cfg_k);
    if (cfg_p)
        cfg_flags = *cfg_p;

    if (cfg_flags & 2) {
        if (!bpf_map_lookup_elem(&target_pids, &pid))
            return 0;
    }
    if (cfg_flags & 4) {
        char cur_comm[TASK_COMM_LEN];
        bpf_get_current_comm(&cur_comm, sizeof(cur_comm));
        if (!bpf_map_lookup_elem(&target_comms, &cur_comm))
            return 0;
    }

    __s32 kernel_stack_id =
        bpf_get_stackid(ctx, &stacks, BPF_F_FAST_STACK_CMP);
    __s32 user_stack_id =
        bpf_get_stackid(ctx, &stacks, BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

    struct il_stack_key key = {};
    key.pid = pid;
    key.tid = tid;
    key.kernel_stack_id = kernel_stack_id;
    key.user_stack_id = user_stack_id;
    bpf_get_current_comm(&key.comm, sizeof(key.comm));

    __u64 *count = bpf_map_lookup_elem(&stack_counts, &key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 init = 1;
        bpf_map_update_elem(&stack_counts, &key, &init, BPF_NOEXIST);
    }

    if (!(cfg_flags & 1))
        return 0;

    struct il_cpu_sample_event *e =
        bpf_ringbuf_reserve(&cpu_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid;
    e->tid = tid;
    e->cpu = bpf_get_smp_processor_id();
    bpf_get_current_comm(&e->comm, sizeof(e->comm));
    e->kernel_stack_id = kernel_stack_id;
    e->user_stack_id = user_stack_id;

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
