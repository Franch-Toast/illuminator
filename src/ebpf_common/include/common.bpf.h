#ifndef __ILLUMINATOR_COMMON_BPF_H
#define __ILLUMINATOR_COMMON_BPF_H

// ============================================================================
// 文件：illuminator/src/ebpf_common/include/common.bpf.h
// ============================================================================
//
// 【作用】
// eBPF 侧（内核态）专用公共头文件。为所有 eBPF 探针程序提供统一的
// 头文件包含路径和全局常量定义。类似于用户态项目中的 "precompiled.h"。
//
// 【工作原理】
// 1. 本头文件仅供 BPF C 编译器（clang -target bpf）使用，不应被用户态
//    代码包含。用户态代码使用 event_types.h。
//
// 2. 核心依赖链：
//    vmlinux.h       → 内核类型定义（由 bpftool btf dump 生成）
//    bpf_helpers.h    → BPF helper 函数声明（bpf_get_current_pid_tgid 等）
//    bpf_tracing.h    → 追踪相关宏和辅助函数（PT_REGS_PARM1 等）
//    bpf_core_read.h  → CO-RE（Compile Once Run Everywhere）安全读取宏
//    event_types.h    → Illuminator 自定义事件结构体
//
// 3. IL_RINGBUF_SIZE 常量定义 ring buffer 共享内存大小（256KB），
//    这是每个探针的 ring buffer 容量上限，过小会丢事件，过大会浪费内存。
//
// 【设计约束】
// - 本文件仅在 -target bpf 编译目标下使用，不能使用 C++ 或系统库头文件
// - 所有 include 必须是 BPF 兼容的头文件
// ============================================================================

// vmlinux.h：包含完整 Linux 内核类型定义（由 CO-RE 机制保证跨内核版本兼容）
// 这是 BPF CO-RE 的核心：一个 vmlinux.h 编译产物可在不同内核版本运行
#include "vmlinux.h"

// bpf_helpers.h：BPF 核心 helper 函数声明
// 提供 bpf_get_current_pid_tgid(), bpf_ktime_get_ns() 等辅助函数
#include <bpf/bpf_helpers.h>

// bpf_tracing.h：BPF 追踪专用宏和辅助函数
// 提供 PT_REGS_PARM1(ctx) 等寄存器参数读取宏，用于 uprobe/uretprobe
#include <bpf/bpf_tracing.h>

// bpf_core_read.h：CO-RE 安全内存读取宏
// 提供 BPF_CORE_READ 等宏，通过 BTF 重定位确保安全读取内核数据结构
// 这是跨内核版本兼容的关键技术
#include <bpf/bpf_core_read.h>

// event_types.h：Illuminator 自定义事件结构体
// 定义 il_cpu_sample_event, il_mem_event 等共享数据结构
#include "event_types.h"

// Ring buffer 共享内存大小：256 KB
// 这是每个 eBPF 探针的 ring buffer 容量上限
// 对于 CPU 采样场景（高频小事件），256KB 在 99Hz 采样率下足够
// 对于 IO 延迟追踪场景（低频大事件），256KB 也能覆盖突发情况
#define IL_RINGBUF_SIZE (256 * 1024)

// ---- Collection Gate: Push 源暂停/恢复控制 ----
// 所有 push-mode BPF 程序应声明此 map 并在 tracepoint 入口调用 CHECK_GATE()。
// 用户态通过 bpf_map_update_elem 将 gate 设为 0（暂停）或 1（恢复）。
// ARRAY map 的 lookup 开销约 5ns，在 10 万次/秒触发率下额外 CPU < 0.05%。
#define DECLARE_COLLECTION_GATE() \
    struct { \
        __uint(type, BPF_MAP_TYPE_ARRAY); \
        __uint(max_entries, 1); \
        __type(key, __u32); \
        __type(value, __u32); \
    } collection_gate SEC(".maps")

#define CHECK_GATE() do { \
    __u32 _gate_key = 0; \
    __u32 *_gate_val = bpf_map_lookup_elem(&collection_gate, &_gate_key); \
    if (!_gate_val || !*_gate_val) return 0; \
} while(0)

// ============================================================================
// Self-observability counters for BPF ring buffer loss and filtering
// ============================================================================
// Each probe that wishes to expose loss statistics declares a PERCPU_ARRAY
// map named "meta_stats" with 4 u64 slots. The user-space side sums the
// per-CPU values to produce cumulative counters.
//
// Slots:
//   0 - total_events   : events that reached the ringbuf send path
//   1 - buffer_full    : ringbuf_reserve failed (buffer full)
//   2 - dropped        : events discarded after reserve (e.g. state filter)
//   3 - filtered       : events dropped by PID/comm filters

#define STAT_TOTAL_EVENTS   0
#define STAT_BUFFER_FULL    1
#define STAT_DROPPED        2
#define STAT_FILTERED       3

#define DECLARE_META_STATS() \
    struct { \
        __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY); \
        __uint(max_entries, 4); \
        __type(key, __u32); \
        __type(value, __u64); \
    } meta_stats SEC(".maps")

#define INC_STAT(stat_key) do { \
    __u32 _key = (stat_key); \
    __u64 *_val = bpf_map_lookup_elem(&meta_stats, &_key); \
    if (_val) __sync_fetch_and_add(_val, 1); \
} while(0)

#endif /* __ILLUMINATOR_COMMON_BPF_H */
