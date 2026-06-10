load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "json_serializer",
    hdrs = ["json_serializer.h"],
    strip_include_prefix = "",
    include_prefix = "serialization",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:data",
        "@nlohmann_json//:json",
    ],
)
