load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:version",
        "//src/core:yaml_config",
        "//src/features:all_drivers",
        "//src/features:feature_registry",
        "//src/plugin:builtin",
        "//src/plugin:manager",
        "//src/serialization:json_serializer",
        "//src/server:http_server",
        "//src/server:api_routes",
        "//src/server:sse_handler",
        "//src/storage:storage",
    ],
)
