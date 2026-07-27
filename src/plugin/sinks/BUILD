load("@rules_cc//cc:defs.bzl", "cc_library")

# ============================================================================
# Sink 插件 BUILD 文件
# ============================================================================
# 用户级 Sink 实现已清除，等待按照新架构规范重新实现。
# 保留 sse_sink 和 recording_sink — 它们是 FeatureDriver 框架的基础设施组件。
#
# 添加新 Sink 时：
#   1. 在子目录（如 console_output/）下创建 xxx_sink.h
#   2. 在本文件中添加 cc_library target
#   3. 在 builtin_plugins.cc 中 #include 头文件
# ============================================================================

cc_library(
    name = "sse_sink",
    hdrs = ["sse_sink/sse_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    deps = [
        "//src/plugin:api",
        "//src/core:common",
        "//src/core:data",
        "//src/core:serialization",
    ],
)

cc_library(
    name = "recording_sink",
    hdrs = ["recording_sink/recording_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
        "//src/core:serialization",
        "@nlohmann_json//:json",
    ],
)
