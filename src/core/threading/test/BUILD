load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "thread_pool_test",
    srcs = ["thread_pool_test.cc"],
    deps = [
        "//src/core:threading",
        "@googletest//:gtest_main",
    ],
)
