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
        "//src/plugin/sources:proc_stat_reader",
        "//src/plugin/sources:cpu_utilization",
        "//src/plugin/sources:process_cpu",
        "//src/plugin/sources:cpu_profiler",
        # Scheduler sources
        "//src/plugin/sources:sched_analyzer",
        "//src/plugin/sources:offcpu_profiler",
        "//src/plugin/sources:ebpf_sched_tracer",
        # I/O + Network
        "//src/plugin/sources:ebpf_io_monitor",
        "//src/plugin/sources:ebpf_net_tracer",
        # Processors
        "//src/plugin/processors:passthrough",
        "//src/plugin/processors:filter",
        "//src/plugin/processors:stack_symbolizer",
        "//src/plugin/processors:stack_merger",
        # Aggregators
        "//src/plugin/aggregators:cpu_stats_aggregator",
        # Sinks
        "//src/plugin/sinks:console_output",
        "//src/plugin/sinks:file_export",
        "//src/plugin/sinks:local_storage",
        "//src/plugin/sinks:pprof_export",
        "//src/plugin/sinks:prometheus_exposition",
        "//src/plugin/sinks:otlp_export",
        # Storage
        "//src/server/storage:sqlite_backend",
    ],
)
