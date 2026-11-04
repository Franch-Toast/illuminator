load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "status_test",
    srcs = ["status_test.cc"],
    deps = [
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    deps = [
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "yaml_config_test",
    srcs = ["yaml_config_test.cc"],
    deps = [
        "//src/core:yaml_config",
        "@googletest//:gtest_main",
    ],
)
