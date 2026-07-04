load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "sqlite_backend_test",
    srcs = ["sqlite_backend_test.cc"],
    deps = [
        "//src/server/storage:sqlite_backend",
        "//src/core:common",
        "//src/core:engine",
        "@googletest//:gtest_main",
    ],
)
