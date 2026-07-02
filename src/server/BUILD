load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "http_server",
    hdrs = ["http_server.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:threading",
        "//third_party/cpp-httplib",
    ],
)

cc_library(
    name = "api_routes",
    hdrs = ["api_routes.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:version",
        "//src/serialization:json_serializer",
        "//src/sinks:recording_sink",
        "//third_party/cpp-httplib",
    ],
)

cc_library(
    name = "sse_handler",
    hdrs = ["sse_handler.h"],
    strip_include_prefix = "",
    include_prefix = "server",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "//src/plugin:api",
        "//third_party/cpp-httplib",
        "@nlohmann_json//:json",
    ],
)
