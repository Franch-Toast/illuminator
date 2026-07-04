load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "pprof_export_sink_test",
    srcs = ["pprof_export_sink_test.cc"],
    deps = [
        "//src/plugin/sinks:pprof_export",
        "//src/core:engine",
        "//src/core:common",
        "@googletest//:gtest_main",
    ],
)
