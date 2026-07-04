load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "console_sink_test",
    srcs = ["console_sink_test.cc"],
    deps = [
        "//src/plugin/sinks:console_output",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
