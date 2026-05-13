cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3", "-lcrypto"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:yaml_config",
        "//src/plugin:builtin",
        "//src/plugin:manager",
        "//src/sources:sched_analyzer",
        "//src/ebpf:loader",
        "//src/server:http_server",
        "//src/server:websocket_server",
        "//src/server:websocket_manager",
        "//src/sinks:prometheus_exposition",
        "//src/storage:storage",
    ],
)
