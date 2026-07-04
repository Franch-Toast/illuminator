load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "otlp_export_sink_test",
    srcs = ["otlp_export_sink_test.cc"],
    deps = [
        "//src/plugin/sinks:otlp_export",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
