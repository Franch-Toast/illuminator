#ifndef __ILLUMINATOR_EVENT_TYPES_H
#define __ILLUMINATOR_EVENT_TYPES_H

// Shared event structures between BPF programs and userspace.
// This file must be compatible with both BPF C and C++ compilers.

#ifndef __cplusplus
// BPF side: types come from vmlinux.h
#else
// Userspace C++ side
#include <cstdint>
#ifndef __u8
typedef uint8_t  il_u8;
typedef uint16_t il_u16;
typedef uint32_t il_u32;
typedef uint64_t il_u64;
typedef int32_t  il_s32;
#define __u8  il_u8
#define __u16 il_u16
#define __u32 il_u32
#define __u64 il_u64
#define __s32 il_s32
#endif
#endif

#define MAX_STACK_DEPTH 127
#define TASK_COMM_LEN 16

struct il_stack_key {
    __u32 pid;
    __u32 tid;
    __s32 kernel_stack_id;
    __s32 user_stack_id;
    char comm[TASK_COMM_LEN];
};

struct il_cpu_sample_event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u32 cpu;
    char comm[TASK_COMM_LEN];
    __s32 kernel_stack_id;
    __s32 user_stack_id;
};

struct il_mem_event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u64 addr;
    __u64 size;
    char comm[TASK_COMM_LEN];
    __u8 alloc;
};

struct il_net_event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u32 protocol;
    __u64 bytes_sent;
    __u64 bytes_recv;
    char comm[TASK_COMM_LEN];
    __u8 event_type;
};

struct il_bio_event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u64 sector;
    __u32 nr_sector;
    __u32 dev;
    __u64 latency_ns;
    char comm[TASK_COMM_LEN];
    __u8 rwflag;
};

struct il_sched_event {
    __u64 timestamp_ns;
    __u32 prev_pid;
    __u32 next_pid;
    __u32 prev_tid;
    __u32 next_tid;
    __u32 cpu;
    __u64 latency_ns;
    char prev_comm[TASK_COMM_LEN];
    char next_comm[TASK_COMM_LEN];
    __u8 event_type;
};

struct il_offcpu_event {
    __u64 timestamp_ns;
    __u32 pid;
    __u32 tid;
    __u32 cpu;
    __u64 duration_ns;
    __s32 kernel_stack_id;
    __s32 user_stack_id;
    char comm[TASK_COMM_LEN];
};

struct il_sched_stats {
    __u64 switch_count;
    __u64 total_runqueue_latency_ns;
    __u64 max_runqueue_latency_ns;
    __u64 migrate_count;
    char comm[TASK_COMM_LEN];
};

#endif /* __ILLUMINATOR_EVENT_TYPES_H */
