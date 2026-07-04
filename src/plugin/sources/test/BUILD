load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "cpu_utilization_test",
    srcs = ["cpu_utilization_test.cc"],
    deps = [
        "//src/plugin/sources:cpu_utilization",
        "//src/core:common",
        "//src/core:data",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)
