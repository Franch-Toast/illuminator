load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:serialization",
        "//src/core:version",
        "//src/core:yaml_config",
        "//src/plugin/features:all_drivers",
        "//src/plugin:infra",
        "//src/plugin:feature_driver_factories",
        "//src/plugin:builtin",
        "//src/server:http_server",
        "//src/server:api_routes",
        "//src/server:api_v2_routes",
        "//src/server:sse_handler",
        "//src/server/storage:storage",
    ],
)
