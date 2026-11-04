load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

cc_test(
    name = "plugin_registry_test",
    srcs = ["plugin_registry_test.cc"],
    deps = [
        "//src/core:common",
        "//src/core:data",
        "//src/plugin:api",
        "//src/plugin:infra",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "test_source_plugin.so",
    testonly = True,
    srcs = ["test_source_plugin.cc"],
    linkshared = True,
    visibility = ["//src/cli:__pkg__"],
    deps = ["//src/plugin:ebpf_source_base"],
)
