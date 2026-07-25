load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "passthrough",
    hdrs = ["passthrough/passthrough_processor.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "filter",
    hdrs = ["filter/filter_processor.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "stack_symbolizer",
    hdrs = ["stack_symbolizer/stack_symbolizer.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:common_utils",
        "//src/plugin:infra",
    ],
)

cc_library(
    name = "stack_merger",
    hdrs = ["stack_merger/stack_merger.h"],
    strip_include_prefix = "",
    include_prefix = "plugin/processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:infra",
    ],
)
