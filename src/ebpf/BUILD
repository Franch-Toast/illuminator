load("@rules_cc//cc:defs.bzl", "cc_library")

exports_files([
    "include/vmlinux.h",
    "include/common.bpf.h",
    "include/event_types.h",
])

cc_library(
    name = "common_bpf_types",
    hdrs = [
        "include/event_types.h",
        "include/bpf_compat.h",
    ],
    strip_include_prefix = "",
    include_prefix = "ebpf",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "stack_trace_util",
    hdrs = ["loader/stack_trace_util.h"],
    strip_include_prefix = "",
    include_prefix = "ebpf",
    visibility = ["//visibility:public"],
    linkopts = ["-lbpf"],
    deps = [
        ":common_bpf_types",
        "//src/core:data",
    ],
)

cc_library(
    name = "loader",
    hdrs = [
        "loader/bpf_program_manager.h",
        "loader/bpf_stats_reader.h",
        "loader/feature_probe.h",
    ],
    strip_include_prefix = "",
    include_prefix = "ebpf",
    visibility = ["//visibility:public"],
    linkopts = ["-lbpf", "-lelf", "-lz"],
    deps = [
        "//src/core:common",
        "//src/plugin:api",
    ],
)
