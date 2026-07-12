load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "feature_driver_test",
    srcs = ["feature_driver_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/plugin/features:cpu_utilization_driver",
        "//src/plugin/features:feature_registry",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "reconfigure_test",
    srcs = ["reconfigure_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/plugin:api",
        "//src/plugin/sinks:recording_sink",
        "@googletest//:gtest_main",
    ],
)
