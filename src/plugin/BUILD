load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "common_utils",
    hdrs = ["common/stack_symbol_resolver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
    ],
)

cc_library(
    name = "api",
    hdrs = [
        "api/plugin_api.h",
        "api/source_plugin.h",
        "api/processor_plugin.h",
        "api/aggregator_plugin.h",
        "api/sink_plugin.h",
        "api/recording_interface.h",
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
    name = "ebpf_source_base",
    hdrs = ["api/ebpf_source_base.h"],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    linkopts = ["-lbpf", "-lelf", "-lz"],
    deps = [
        ":api",
        "//src/core:common",
        "//src/ebpf_common:loader",
    ],
)

cc_library(
    name = "infra",
    hdrs = [
        "infra/plugin_registry.h",
        "infra/so_loader.h",
        "infra/plugin_manager.h",
    ],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    linkopts = ["-ldl"],
    deps = [
        ":api",
        "//src/core:common",
        "//src/core:engine",
    ],
)

cc_library(
    name = "feature_driver_factories",
    hdrs = ["infra/feature_driver_factories.h"],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    deps = [
        ":api",
        "//src/core:engine",
        "//src/plugin/sinks:recording_sink",
        "//src/plugin/sinks:sse_sink",
    ],
)

cc_library(
    name = "builtin",
    srcs = ["infra/builtin_plugins.cc"],
    hdrs = ["infra/builtin_plugins.h"],
    strip_include_prefix = "",
    include_prefix = "plugin",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":api",
        ":infra",
        # 添加新插件的 BUILD target 到这里
    ],
)
