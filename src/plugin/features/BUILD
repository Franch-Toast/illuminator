load("@rules_cc//cc:defs.bzl", "cc_library")
load("//src/ebpf_common:bpf_probe.bzl", "bpf_skeleton")

# ============================================================================
# BPF Skeleton compilation (probe .bpf.c → skeleton .skel.h)
# ============================================================================

bpf_skeleton(name = "cpu_profiler_sk", src = "cpu/cpu_profiler/cpu_profiler.bpf.c")
bpf_skeleton(name = "sched_analyzer_sk", src = "sched/sched_analyzer/sched_analyzer.bpf.c")
bpf_skeleton(name = "offcpu_profiler_sk", src = "sched/offcpu_profiler/offcpu_profiler.bpf.c")
bpf_skeleton(name = "bio_latency_sk", src = "io/io_monitor/bio_latency.bpf.c")
bpf_skeleton(name = "net_tracer_sk", src = "net/net_tracer/net_tracer.bpf.c")

# ============================================================================
# CPU subsystem — cpu_profiler (eBPF)
# ============================================================================

cc_library(
    name = "cpu_profiler_source",
    hdrs = ["cpu/cpu_profiler/cpu_profiler_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":cpu_profiler_sk_skel",
        "//src/plugin:ebpf_source_base",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/ebpf_common:loader",
        "//src/ebpf_common:stack_trace_util",
        "//src/plugin:api",
        "//src/plugin:infra",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "cpu_profiler_driver",
    hdrs = ["cpu/cpu_profiler/cpu_profiler_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":cpu_profiler_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
        "//src/plugin/processors:stack_symbolizer",
    ],
)

# ============================================================================
# CPU subsystem — cpu_utilization (proc-based)
# ============================================================================

cc_library(
    name = "proc_stat_reader",
    hdrs = ["cpu/cpu_utilization/proc_stat_reader.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "cpu_utilization_source",
    hdrs = ["cpu/cpu_utilization/cpu_utilization_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "cpu_utilization_driver",
    hdrs = ["cpu/cpu_utilization/cpu_utilization_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":cpu_utilization_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# CPU subsystem — process_cpu (proc-based)
# ============================================================================

cc_library(
    name = "process_cpu_source",
    hdrs = ["cpu/process_cpu/process_cpu_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "process_cpu_driver",
    hdrs = ["cpu/process_cpu/process_cpu_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":process_cpu_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# I/O subsystem — io_monitor (eBPF)
# ============================================================================

cc_library(
    name = "io_monitor_source",
    hdrs = ["io/io_monitor/io_monitor_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":bio_latency_sk_skel",
        "//src/plugin:ebpf_source_base",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "io_monitor_driver",
    hdrs = ["io/io_monitor/io_monitor_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":io_monitor_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# Network subsystem — net_tracer (eBPF)
# ============================================================================

cc_library(
    name = "net_tracer_source",
    hdrs = ["net/net_tracer/net_tracer_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":net_tracer_sk_skel",
        "//src/plugin:ebpf_source_base",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "net_tracer_driver",
    hdrs = ["net/net_tracer/net_tracer_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":net_tracer_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# Scheduler subsystem — sched_analyzer (eBPF)
# ============================================================================

cc_library(
    name = "sched_analyzer_source",
    hdrs = ["sched/sched_analyzer/sched_analyzer_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":sched_analyzer_sk_skel",
        "//src/plugin:ebpf_source_base",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/plugin:api",
        "//src/plugin:infra",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "sched_analyzer_driver",
    hdrs = ["sched/sched_analyzer/sched_analyzer_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":sched_analyzer_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# Scheduler subsystem — offcpu_profiler (eBPF)
# ============================================================================

cc_library(
    name = "offcpu_profiler_source",
    hdrs = ["sched/offcpu_profiler/offcpu_profiler_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":offcpu_profiler_sk_skel",
        "//src/plugin:ebpf_source_base",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/ebpf_common:loader",
        "//src/ebpf_common:stack_trace_util",
        "//src/plugin:api",
        "//src/plugin:infra",
        "//src/plugin/processors:stack_symbolizer",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "offcpu_profiler_driver",
    hdrs = ["sched/offcpu_profiler/offcpu_profiler_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":offcpu_profiler_source",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/core:engine",
    ],
)

# ============================================================================
# Aggregation targets
# ============================================================================

cc_library(
    name = "all_sources",
    visibility = ["//visibility:public"],
    deps = [
        ":cpu_profiler_source",
        ":cpu_utilization_source",
        ":io_monitor_source",
        ":net_tracer_source",
        ":offcpu_profiler_source",
        ":proc_stat_reader",
        ":process_cpu_source",
        ":sched_analyzer_source",
    ],
)

cc_library(
    name = "all_drivers",
    visibility = ["//visibility:public"],
    deps = [
        ":cpu_profiler_driver",
        ":cpu_utilization_driver",
        ":io_monitor_driver",
        ":net_tracer_driver",
        ":offcpu_profiler_driver",
        ":process_cpu_driver",
        ":sched_analyzer_driver",
    ],
)
