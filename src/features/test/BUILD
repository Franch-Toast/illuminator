load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "feature_driver_test",
    srcs = ["feature_driver_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/features:cpu_utilization_driver",
        "//src/features:feature_registry",
        "@googletest//:gtest_main",
    ],
)
