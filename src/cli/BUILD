load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:yaml_config",
        "//src/plugin:builtin",
        "//src/plugin:manager",
        "//src/serialization:json_serializer",
        "//src/server:http_server",
        "//src/server:api_routes",
        "//src/server:websocket_manager",
        "//src/storage:storage",
    ],
)
