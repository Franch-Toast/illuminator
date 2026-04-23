cc_library(
    name = "common_bpf_types",
    hdrs = ["include/event_types.h"],
    strip_include_prefix = "",
    include_prefix = "ebpf",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "loader",
    hdrs = [
        "loader/bpf_program_manager.h",
        "loader/feature_probe.h",
    ],
    strip_include_prefix = "",
    include_prefix = "ebpf",
    visibility = ["//visibility:public"],
    linkopts = ["-lbpf", "-lelf", "-lz"],
    deps = [
        "//src/core:common",
    ],
)
