#include "../include/common.bpf.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} mem_events SEC(".maps");

// Track outstanding allocations for leak detection
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u64);  // address
    __type(value, struct il_mem_event);
} allocs SEC(".maps");

// Temporary storage for malloc return probes
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u64);  // tid
    __type(value, __u64);  // requested size
} alloc_sizes SEC(".maps");

SEC("uprobe")
int uprobe_malloc(struct pt_regs *ctx) {
    __u64 size = PT_REGS_PARM1(ctx);
    __u64 tid = bpf_get_current_pid_tgid();
    bpf_map_update_elem(&alloc_sizes, &tid, &size, BPF_ANY);
    return 0;
}

SEC("uretprobe")
int uretprobe_malloc(struct pt_regs *ctx) {
    __u64 tid = bpf_get_current_pid_tgid();
    __u64 *size = bpf_map_lookup_elem(&alloc_sizes, &tid);
    if (!size) return 0;

    __u64 addr = PT_REGS_RC(ctx);
    if (addr == 0) {
        bpf_map_delete_elem(&alloc_sizes, &tid);
        return 0;
    }

    struct il_mem_event *e = bpf_ringbuf_reserve(&mem_events, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&alloc_sizes, &tid);
        return 0;
    }

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;
    e->tid = (__u32)pid_tgid;
    e->addr = addr;
    e->size = *size;
    e->alloc = 1;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // Track allocation
    bpf_map_update_elem(&allocs, &addr, e, BPF_ANY);

    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&alloc_sizes, &tid);
    return 0;
}

SEC("uprobe")
int uprobe_free(struct pt_regs *ctx) {
    __u64 addr = PT_REGS_PARM1(ctx);
    if (addr == 0) return 0;

    struct il_mem_event *alloc = bpf_map_lookup_elem(&allocs, &addr);
    if (!alloc) return 0;

    struct il_mem_event *e = bpf_ringbuf_reserve(&mem_events, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&allocs, &addr);
        return 0;
    }

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;
    e->tid = (__u32)pid_tgid;
    e->addr = addr;
    e->size = alloc->size;
    e->alloc = 0;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    bpf_map_delete_elem(&allocs, &addr);
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
