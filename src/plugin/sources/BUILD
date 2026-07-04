load("@rules_cc//cc:defs.bzl", "cc_library")

# Source plugins — organized by subsystem (cpu, sched, io, net)

# ============================================================================
# eBPF common base class
# ============================================================================

cc_library(
    name = "ebpf_ring_buffer_source",
    hdrs = ["ebpf_ring_buffer_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

# ============================================================================
# CPU subsystem
# ============================================================================

cc_library(
    name = "proc_stat_reader",
    hdrs = ["cpu/proc_stat_reader/proc_stat_reader.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)


cc_library(
    name = "cpu_utilization",
    hdrs = ["cpu/cpu_utilization/cpu_utilization.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "process_cpu",
    hdrs = ["cpu/process_cpu/process_cpu.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "cpu_profiler",
    hdrs = ["cpu/cpu_profiler/cpu_profiler.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/ebpf:stack_trace_util",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

# ============================================================================
# Scheduler subsystem
# ============================================================================

cc_library(
    name = "sched_analyzer",
    hdrs = ["sched/sched_analyzer/sched_analyzer.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "ebpf_sched_tracer",
    hdrs = ["sched/ebpf_sched_tracer/ebpf_sched_tracer.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":ebpf_ring_buffer_source",
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "offcpu_profiler",
    hdrs = ["sched/offcpu_profiler/offcpu_profiler.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/ebpf:stack_trace_util",
        "//src/plugin/processors:stack_symbolizer",
        "//src/plugin:api",
        "//src/plugin:manager",
        "@nlohmann_json//:json",
    ],
)

# ============================================================================
# I/O subsystem
# ============================================================================

cc_library(
    name = "ebpf_io_monitor",
    hdrs = ["io/ebpf_io_monitor/ebpf_io_monitor.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":ebpf_ring_buffer_source",
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

# ============================================================================
# Network subsystem
# ============================================================================

cc_library(
    name = "ebpf_net_tracer",
    hdrs = ["net/ebpf_net_tracer/ebpf_net_tracer.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":ebpf_ring_buffer_source",
        "//src/core:common",
        "//src/core:threading",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
