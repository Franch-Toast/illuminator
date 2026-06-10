load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "server_test",
    srcs = ["server_test.cc"],
    deps = [
        "//src/server:api_routes",
        "//src/server:http_server",
        "//src/core:common",
        "//src/core:engine",
        "//src/core:version",
        "//third_party/cpp-httplib",
        "@nlohmann_json//:json",
        "@googletest//:gtest_main",
    ],
    linkopts = ["-pthread"],
)
