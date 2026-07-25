load("@rules_cc//cc:defs.bzl", "cc_library")

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
        "//src/cli:json_serializer",
    ],
)

cc_library(
    name = "console_output",
    hdrs = ["console_output/console_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
        "//src/cli:json_serializer",
    ],
)

cc_library(
    name = "file_export",
    hdrs = ["file_export/file_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
        "//src/cli:json_serializer",
    ],
)

cc_library(
    name = "local_storage",
    hdrs = ["local_storage/local_storage_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
        "//src/server/storage:storage",
        "//src/server/storage:sqlite_backend",
    ],
)

cc_library(
    name = "pprof_export",
    hdrs = ["pprof_export/pprof_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "otlp_export",
    hdrs = ["otlp_export/otlp_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "prometheus_exposition",
    hdrs = ["prometheus_exposition/prometheus_sink.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)


cc_library(
    name = "fanout",
    hdrs = ["fanout/sink_fanout.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
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
        "//src/cli:json_serializer",
        "@nlohmann_json//:json",
    ],
)
