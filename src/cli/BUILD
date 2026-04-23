cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread", "-lbpf", "-lelf", "-lz", "-lsqlite3"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/plugin:builtin",
        "//src/plugin:manager",
        "//src/server:http_server",
        "//src/sinks:prometheus_exposition",
        "//src/storage:storage",
    ],
)
