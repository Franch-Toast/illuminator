load("@rules_cc//cc:defs.bzl", "cc_library")

# Standalone recording output; independent of any application or server.
cc_library(
    name = "recording_sink",
    hdrs = ["recording_sink/recording_sink.h"],
    include_prefix = "plugin/sinks",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:serialization",
        "//src/plugin:api",
        "@nlohmann_json//:json",
    ],
)
