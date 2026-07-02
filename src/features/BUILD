load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "feature_registry",
    hdrs = ["feature_registry.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:engine",
    ],
)

cc_library(
    name = "cpu_utilization_driver",
    hdrs = ["cpu_utilization_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:cpu_utilization",
    ],
)

cc_library(
    name = "process_cpu_driver",
    hdrs = ["process_cpu_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:process_cpu",
    ],
)

cc_library(
    name = "cpu_profiler_driver",
    hdrs = ["cpu_profiler_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:cpu_profiler",
    ],
)

cc_library(
    name = "io_monitor_driver",
    hdrs = ["io_monitor_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:ebpf_io_monitor",
    ],
)

cc_library(
    name = "net_tracer_driver",
    hdrs = ["net_tracer_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:ebpf_net_tracer",
    ],
)

cc_library(
    name = "sched_analyzer_driver",
    hdrs = ["sched_analyzer_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:sched_analyzer",
    ],
)

cc_library(
    name = "offcpu_profiler_driver",
    hdrs = ["offcpu_profiler_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/sources:offcpu_profiler",
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
