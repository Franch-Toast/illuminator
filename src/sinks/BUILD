cc_library(
    name = "console_output",
    hdrs = ["console_output/console_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "file_export",
    hdrs = ["file_export/file_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "local_storage",
    hdrs = ["local_storage/local_storage_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
        "//src/storage:storage",
        "//src/storage:sqlite_backend",
    ],
)

cc_library(
    name = "pprof_export",
    hdrs = ["pprof_export/pprof_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "otlp_export",
    hdrs = ["otlp_export/otlp_export_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "prometheus_exposition",
    hdrs = ["prometheus_exposition/prometheus_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "websocket_sink",
    hdrs = ["websocket_sink/websocket_sink.h"],
    strip_include_prefix = "",
    include_prefix = "sinks",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
