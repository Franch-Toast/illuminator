load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# ============================================================================
# Feature 插件 BUILD 文件
# ============================================================================
# 添加新 Feature 时：
#   1. 在对应子目录（cpu/sched/io/net/...）下创建 xxx_source.h 和 xxx_driver.h
#   2. 在本文件中添加 bpf_skeleton (eBPF) 和 cc_library targets
#   3. 将 Source target 加入 all_sources deps
#   4. 将 Driver target 加入 all_drivers deps (alwayslink = True)
# ============================================================================

# ---- CPU Utilization ----
cc_library(
    name = "cpu_utilization_source",
    hdrs = ["cpu/cpu_utilization/cpu_utilization_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "cpu_utilization_driver",
    srcs = ["cpu/cpu_utilization/register.cc"],
    hdrs = ["cpu/cpu_utilization/cpu_utilization_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    alwayslink = True,
    deps = [
        ":cpu_utilization_source",
        "//src/core:engine",
        "//src/plugin:infra",
        "//src/plugin:feature_driver_factories",
    ],
)

cc_test(
    name = "cpu_utilization_test",
    srcs = ["cpu/cpu_utilization/cpu_utilization_test.cc"],
    deps = [
        ":cpu_utilization_source",
        "//src/core:common",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)

# ---- Process CPU ----
cc_library(
    name = "process_cpu_source",
    hdrs = ["cpu/process_cpu/process_cpu_source.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    alwayslink = True,
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "process_cpu_driver",
    srcs = ["cpu/process_cpu/register.cc"],
    hdrs = ["cpu/process_cpu/process_cpu_driver.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/features",
    alwayslink = True,
    deps = [
        ":process_cpu_source",
        "//src/core:engine",
        "//src/plugin:infra",
        "//src/plugin:feature_driver_factories",
    ],
)

cc_test(
    name = "process_cpu_test",
    srcs = ["cpu/process_cpu/process_cpu_test.cc"],
    deps = [
        ":process_cpu_source",
        "//src/core:common",
        "//src/plugin:api",
        "@googletest//:gtest_main",
    ],
)

# ---- 聚合 targets ----
cc_library(
    name = "all_sources",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":cpu_utilization_source",
        ":process_cpu_source",
    ],
)

cc_library(
    name = "all_drivers",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        ":cpu_utilization_driver",
        ":process_cpu_driver",
    ],
)
