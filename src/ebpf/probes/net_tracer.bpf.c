#include "../include/common.bpf.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} net_events SEC(".maps");

SEC("tracepoint/sock/inet_sock_set_state")
int trace_inet_sock_set_state(struct trace_event_raw_inet_sock_set_state *ctx) {
    __u16 family = ctx->family;
    if (family != 2 /* AF_INET */) return 0;

    struct il_net_event *e = bpf_ringbuf_reserve(&net_events, sizeof(*e), 0);
    if (!e) return 0;

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;
    e->tid = (__u32)pid_tgid;
    e->protocol = ctx->protocol;
    e->sport = ctx->sport;
    e->dport = ctx->dport;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    __builtin_memcpy(&e->saddr, ctx->saddr, 4);
    __builtin_memcpy(&e->daddr, ctx->daddr, 4);

    int newstate = ctx->newstate;
    int oldstate = ctx->oldstate;

    if (newstate == 1 /* TCP_ESTABLISHED */) {
        e->event_type = 0;  // connect/accept
    } else if (newstate == 7 /* TCP_CLOSE */) {
        e->event_type = 2;  // close
    } else {
        bpf_ringbuf_discard(e, 0);
        return 0;
    }

    e->bytes_sent = 0;
    e->bytes_recv = 0;

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
