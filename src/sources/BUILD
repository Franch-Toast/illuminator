cc_library(
    name = "proc_stat_reader",
    hdrs = ["proc_stat_reader/proc_stat_reader.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "ebpf_cpu_sampler",
    hdrs = ["ebpf_cpu_sampler/ebpf_cpu_sampler.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "ebpf_net_tracer",
    hdrs = ["ebpf_net_tracer/ebpf_net_tracer.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "ebpf_io_monitor",
    hdrs = ["ebpf_io_monitor/ebpf_io_monitor.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "ebpf_sched_tracer",
    hdrs = ["ebpf_sched_tracer/ebpf_sched_tracer.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/ebpf:common_bpf_types",
        "//src/ebpf:loader",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
