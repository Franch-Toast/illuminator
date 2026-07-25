load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "json_serializer",
    hdrs = ["json_serializer.h"],
    strip_include_prefix = "",
    include_prefix = "cli",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "@nlohmann_json//:json",
    ],
)

cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:version",
        "//src/core:yaml_config",
        "//src/plugin/features:all_drivers",
        "//src/plugin:infra",
        "//src/plugin:feature_registry",
        "//src/plugin:builtin",
        ":json_serializer",
        "//src/server:http_server",
        "//src/server:api_routes",
        "//src/server:sse_handler",
        "//src/server/storage:storage",
    ],
)
