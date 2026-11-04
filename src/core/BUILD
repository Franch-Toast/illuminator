load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:version.bzl", "version_header")

version_header(
    name = "gen_version_header",
    out = "common/version_generated.h",
)

cc_library(
    name = "version",
    hdrs = ["common/version_generated.h"],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "common",
    hdrs = [
        "common/config.h",
        "common/logging.h",
        "common/proc_reader.h",
        "common/status.h",
        "common/string_util.h",
    ],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        "@nlohmann_json//:json",
        "@spdlog",
    ],
)

cc_library(
    name = "memory",
    hdrs = [
        "memory/arena.h",
        "memory/lock_free_queue.h",
    ],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "threading",
    hdrs = [
        "threading/thread_pool.h",
        "threading/thread_util.h",
    ],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "data",
    hdrs = [
        "common/data_batch.h",
    ],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        ":memory",
    ],
)

cc_library(
    name = "serialization",
    hdrs = ["common/json_serializer.h"],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":data",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "engine",
    hdrs = [
        "engine/async_channel.h",
        "engine/infrastructure_manager.h",
        "engine/pid_manager.h",
        "engine/pipeline.h",
        "engine/self_observability.h",
        "engine/timer_wheel.h",
    ],
    include_prefix = "core",
    linkopts = ["-lbpf"],
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        ":data",
        ":memory",
        ":threading",
        "//src/plugin:api",
    ],
)

cc_library(
    name = "yaml_config",
    hdrs = [
        "common/yaml_config_loader.h",
    ],
    include_prefix = "core",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":common",
        "@yaml_cpp//:yaml-cpp",
    ],
)
