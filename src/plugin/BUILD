load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "api",
    hdrs = [
        "api/plugin_api.h",
        "api/source_plugin.h",
        "api/processor_plugin.h",
        "api/aggregator_plugin.h",
        "api/sink_plugin.h",
    ],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "//src/core:memory",
    ],
)

cc_library(
    name = "manager",
    hdrs = [
        "manager/plugin_registry.h",
        "manager/so_loader.h",
        "manager/plugin_manager.h",
    ],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    linkopts = ["-ldl"],
    deps = [
        ":api",
        "//src/core:common",
    ],
)

cc_library(
    name = "builtin",
    srcs = ["builtin/builtin_plugins.cc"],
    hdrs = ["builtin/builtin_plugins.h"],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":api",
        ":manager",
        # CPU sources
        "//src/sources:proc_stat_reader",
        "//src/sources:cpu_utilization",
        "//src/sources:process_cpu",
        "//src/sources:cpu_profiler",
        # Scheduler sources
        "//src/sources:sched_analyzer",
        "//src/sources:offcpu_profiler",
        "//src/sources:ebpf_sched_tracer",
        # I/O + Network
        "//src/sources:ebpf_io_monitor",
        "//src/sources:ebpf_net_tracer",
        # Processors
        "//src/processors:passthrough",
        "//src/processors:filter",
        "//src/processors:stack_symbolizer",
        "//src/processors:stack_merger",
        # Aggregators
        "//src/aggregators:cpu_stats_aggregator",
        # Sinks
        "//src/sinks:console_output",
        "//src/sinks:file_export",
        "//src/sinks:local_storage",
        "//src/sinks:pprof_export",
        "//src/sinks:prometheus_exposition",
        "//src/sinks:otlp_export",
        # Storage
        "//src/storage:sqlite_backend",
    ],
)
