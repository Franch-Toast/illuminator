load("@rules_cc//cc:defs.bzl", "cc_library")

# ============================================================================
# Feature 插件 BUILD 文件
# ============================================================================
# 所有具体 Feature 实现已清除，等待按照新架构规范重新实现。
#
# 添加新 Feature 时：
#   1. 在对应子目录（cpu/sched/io/net/...）下创建 xxx_source.h 和 xxx_driver.h
#   2. 在本文件中添加 bpf_skeleton (eBPF) 和 cc_library targets
#   3. 将 Source target 加入 all_sources deps
#   4. 将 Driver target 加入 all_drivers deps
# ============================================================================

cc_library(
    name = "all_sources",
    visibility = ["//visibility:public"],
    deps = [],
)

cc_library(
    name = "all_drivers",
    visibility = ["//visibility:public"],
    deps = [],
)
