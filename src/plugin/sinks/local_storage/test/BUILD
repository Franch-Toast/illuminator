load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "local_storage_sink_test",
    srcs = ["local_storage_sink_test.cc"],
    deps = [
        "//src/plugin/sinks:local_storage",
        "//src/server/storage:sqlite_backend",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
