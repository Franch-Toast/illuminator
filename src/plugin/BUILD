load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "common_utils",
    hdrs = ["common/stack_symbol_resolver.h"],
    include_prefix = "plugin",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
    ],
)

cc_library(
    name = "api",
    hdrs = [
        "api/aggregator_plugin.h",
        "api/plugin_api.h",
        "api/processor_plugin.h",
        "api/recording_interface.h",
        "api/sink_plugin.h",
        "api/source_plugin.h",
    ],
    include_prefix = "plugin",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "//src/core:memory",
    ],
)

cc_library(
    name = "ebpf_source_base",
    hdrs = ["api/ebpf_source_base.h"],
    include_prefix = "plugin",
    linkopts = [
        "-lbpf",
        "-lelf",
        "-lz",
    ],
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":api",
        "//src/core:common",
        "//src/ebpf_common:common_bpf_types",
        "//src/ebpf_common:loader",
    ],
)

cc_library(
    name = "infra",
    hdrs = [
        "infra/plugin_manager.h",
        "infra/plugin_registry.h",
        "infra/so_loader.h",
    ],
    include_prefix = "plugin",
    linkopts = ["-ldl"],
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":api",
        "//src/core:common",
    ],
)

cc_library(
    name = "builtin",
    srcs = ["infra/builtin_plugins.cc"],
    hdrs = ["infra/builtin_plugins.h"],
    include_prefix = "plugin",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":api",
        ":infra",
        "//src/plugin/sinks:recording_sink",
        # 添加新插件的 BUILD target 到这里
    ],
    alwayslink = True,
)
