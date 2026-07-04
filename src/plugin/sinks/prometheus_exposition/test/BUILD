load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "prometheus_sink_test",
    srcs = ["prometheus_sink_test.cc"],
    deps = [
        "//src/plugin/sinks:prometheus_exposition",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
