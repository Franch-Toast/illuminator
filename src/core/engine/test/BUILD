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
        "//src/core:common",
        "//src/core:engine",
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
    name = "pid_manager_test",
    srcs = ["pid_manager_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pipeline_dynamic_sink_test",
    srcs = ["pipeline_dynamic_sink_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "stress_test",
    srcs = ["stress_test.cc"],
    deps = [
        "//src/core:engine",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pipeline_shutdown_test",
    srcs = ["pipeline_shutdown_test.cc"],
    deps = [
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)
