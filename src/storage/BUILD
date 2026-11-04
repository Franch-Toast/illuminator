load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "storage",
    hdrs = [
        "storage_backend.h",
    ],
    include_prefix = "storage",
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
    ],
)

cc_library(
    name = "sqlite_backend",
    hdrs = ["sqlite_backend/sqlite_backend.h"],
    include_prefix = "storage",
    linkopts = ["-lsqlite3"],
    strip_include_prefix = "",
    visibility = ["//visibility:public"],
    deps = [
        ":storage",
        "//src/core:common",
        "@nlohmann_json//:json",
    ],
    alwayslink = True,
)
