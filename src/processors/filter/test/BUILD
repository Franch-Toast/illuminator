load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "filter_processor_test",
    srcs = ["filter_processor_test.cc"],
    deps = [
        "//src/processors:filter",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
