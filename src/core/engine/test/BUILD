load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "timer_wheel_test",
    srcs = ["timer_wheel_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "async_channel_test",
    srcs = ["async_channel_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "data_batch_test",
    srcs = ["data_batch_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pipeline_integration_test",
    srcs = ["pipeline_integration_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/core:common",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)


cc_test(
    name = "infrastructure_manager_test",
    srcs = ["infrastructure_manager_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "feature_bus_test",
    srcs = ["feature_bus_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)
