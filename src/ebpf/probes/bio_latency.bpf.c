#include "../include/common.bpf.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} bio_events SEC(".maps");

struct ns_request_start {
    __u64 start_ns;
    __u32 pid;
    __u32 tid;
    char comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);  // request pointer
    __type(value, struct ns_request_start);
} req_starts SEC(".maps");

SEC("tracepoint/block/block_rq_issue")
int trace_block_rq_issue(struct trace_event_raw_block_rq *ctx) {
    __u64 key = ctx->sector;
    struct ns_request_start start = {};
    start.start_ns = bpf_ktime_get_ns();

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    start.pid = pid_tgid >> 32;
    start.tid = (__u32)pid_tgid;
    bpf_get_current_comm(&start.comm, sizeof(start.comm));

    bpf_map_update_elem(&req_starts, &key, &start, BPF_ANY);
    return 0;
}

SEC("tracepoint/block/block_rq_complete")
int trace_block_rq_complete(struct trace_event_raw_block_rq_complete *ctx) {
    __u64 key = ctx->sector;
    struct ns_request_start *start = bpf_map_lookup_elem(&req_starts, &key);
    if (!start) return 0;

    struct il_bio_event *e = bpf_ringbuf_reserve(&bio_events, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&req_starts, &key);
        return 0;
    }

    __u64 now = bpf_ktime_get_ns();
    e->timestamp_ns = now;
    e->pid = start->pid;
    e->tid = start->tid;
    e->latency_ns = now - start->start_ns;
    e->sector = ctx->sector;
    e->nr_sector = ctx->nr_sector;
    __builtin_memcpy(&e->comm, &start->comm, TASK_COMM_LEN);
    e->rwflag = (ctx->rwbs[0] == 'W') ? 1 : 0;

    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&req_starts, &key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
