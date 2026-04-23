cc_library(
    name = "passthrough",
    hdrs = ["passthrough/passthrough_processor.h"],
    strip_include_prefix = "",
    include_prefix = "processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)

cc_library(
    name = "filter",
    hdrs = ["filter/filter_processor.h"],
    strip_include_prefix = "",
    include_prefix = "processors",
    visibility = ["//visibility:public"],
    alwayslink = True,
    deps = [
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
