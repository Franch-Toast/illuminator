cc_library(
    name = "http_server",
    hdrs = ["http_server.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
    ],
)

cc_library(
    name = "websocket_server",
    hdrs = ["websocket_server.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    linkopts = ["-lcrypto"],
    deps = [
        "//src/core:common",
    ],
)

cc_library(
    name = "websocket_manager",
    hdrs = ["websocket_manager.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    deps = [
        ":websocket_server",
        "//src/core:common",
        "//src/sinks:websocket_sink",
    ],
)
