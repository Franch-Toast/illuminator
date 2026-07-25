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
        "common/data_batch.h",
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
    hdrs = [
        "engine/async_channel.h",
        "engine/feature_bus.h",
        "engine/feature_driver.h",
        "engine/infrastructure_manager.h",
        "engine/pid_manager.h",
        "engine/pipeline.h",
        "engine/timer_wheel.h",
        "engine/self_observability.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    linkopts = ["-lbpf"],
    deps = [
        ":common",
        ":data",
        ":memory",
        ":threading",
        "//src/plugin:api",
        "//src/plugin/sinks:recording_sink",
        "//src/plugin/sinks:sse_sink",
        "//src/server/storage:storage",
    ],
)

cc_library(
    name = "yaml_config",
    hdrs = [
        "common/yaml_config_loader.h",
    ],
    strip_include_prefix = "",
    include_prefix = "core",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        "@yaml_cpp//:yaml-cpp",
    ],
)
