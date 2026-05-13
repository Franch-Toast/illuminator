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
        "manager/wasm_runtime.h",
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
        # Legacy sources
        "//src/sources:proc_stat_reader",
        "//src/sources:cpu_sys_monitor",
        "//src/sources:process_cpu_monitor",
        "//src/sources:ebpf_cpu_sampler",
        "//src/sources:ebpf_net_tracer",
        "//src/sources:ebpf_io_monitor",
        "//src/sources:ebpf_sched_tracer",
        "//src/sources:cpu_sys_stats",
        "//src/sources:proc_cpu_monitor",
        # New CPU monitoring sources
        "//src/sources:cpu_utilization",
        "//src/sources:process_cpu",
        "//src/sources:cpu_profiler",
        "//src/sources:sched_analyzer",
        "//src/sources:offcpu_profiler",
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
        "//src/sinks:websocket_sink",
        # Storage
        "//src/storage:sqlite_backend",
    ],
)
