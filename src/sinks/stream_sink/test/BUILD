load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "stream_sink_test",
    srcs = ["stream_sink_test.cc"],
    deps = [
        "//src/sinks:stream_sink",
        "//src/core:data",
        "@googletest//:gtest_main",
    ],
)
