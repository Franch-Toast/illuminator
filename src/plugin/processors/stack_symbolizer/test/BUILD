load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "stack_symbolizer_processor_test",
    srcs = ["stack_symbolizer_processor_test.cc"],
    deps = [
        "//src/plugin/processors:stack_symbolizer",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
