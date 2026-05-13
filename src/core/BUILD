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
        "threading/thread_pool.h",
        "threading/thread_util.h",
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
        "engine/pipeline_controller.h",
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
