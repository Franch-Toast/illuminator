load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "sink_fanout_test",
    srcs = ["sink_fanout_test.cc"],
    deps = [
        "//src/plugin/sinks:fanout",
        "//src/core:data",
        "@googletest//:gtest_main",
    ],
)
