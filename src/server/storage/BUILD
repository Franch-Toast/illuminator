load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "storage",
    hdrs = [
        "storage_backend.h",
    ],
    strip_include_prefix = "",
    include_prefix = "server/storage",
    visibility = ["//visibility:public"],
    deps = [
        "//src/core:common",
        "//src/core:data",
    ],
)

cc_library(
    name = "sqlite_backend",
    hdrs = ["sqlite_backend/sqlite_backend.h"],
    strip_include_prefix = "",
    include_prefix = "server/storage",
    visibility = ["//visibility:public"],
    alwayslink = True,
    linkopts = ["-lsqlite3"],
    deps = [
        ":storage",
        "//src/core:common",
        "@nlohmann_json//:json",
    ],
)
