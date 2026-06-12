load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:version.bzl", "version_header")

version_header(
    name = "gen_version_header",
    out = "common/version_generated.h",
)

cc_library(
    name = "version",
    hdrs = ["common/version_generated.h"],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "common",
    hdrs = [
        "common/status.h",
        "common/logging.h",
        "common/config.h",
        "common/self_observability.h",
        "common/string_util.h",
        "common/proc_reader.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    deps = [
        "@spdlog",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "memory",
    hdrs = [
        "memory/arena.h",
        "memory/lock_free_queue.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "threading",
    hdrs = [
        "threading/thread_util.h",
        "threading/thread_pool.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "data",
    hdrs = [
        "engine/data_batch.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        ":memory",
    ],
)

cc_library(
    name = "engine",
    srcs = ["engine/pipeline_controller.cc"],
    hdrs = [
        "engine/async_channel.h",
        "engine/feature_manager.h",
        "engine/pipeline_controller.h",
        "engine/timer_wheel.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        ":data",
        ":memory",
        ":threading",
        "//src/plugin:api",
        "//src/plugin:manager",
        "//src/sinks:recording_sink",
        "//src/sinks:stream_sink",
        "//src/sinks:websocket_sink",
        "//src/storage:storage",
    ],
)

cc_library(
    name = "yaml_config",
    hdrs = [
        "config/yaml_config_loader.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        "@yaml_cpp//:yaml-cpp",
    ],
)
