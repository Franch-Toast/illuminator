load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "sqlite_backend_test",
    srcs = ["sqlite_backend_test.cc"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "//src/storage:sqlite_backend",
        "@googletest//:gtest_main",
    ],
)
