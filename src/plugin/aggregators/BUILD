load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "cpu_stats_aggregator",
    hdrs = ["cpu_stats_aggregator/cpu_stats_aggregator.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/aggregators",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
