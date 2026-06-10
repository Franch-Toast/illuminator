load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "plugin_registry_test",
    srcs = ["plugin_registry_test.cc"],
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
        "//src/core:common",
        "//src/core:data",
        "@googletest//:gtest_main",
    ],
)
