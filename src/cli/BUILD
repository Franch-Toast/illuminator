load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "illuminator",
    srcs = ["main.cc"],
    linkopts = ["-pthread"],
    deps = [
        "//src/core:common",
        "//src/core:engine",
        "//src/core:version",
        "//src/core:yaml_config",
        "//src/plugin:builtin",
        "//src/plugin:infra",
        "//src/plugin/sinks:recording_sink",
        "//src/storage:sqlite_backend",
    ],
)

sh_test(
    name = "smoke_test",
    srcs = ["smoke_test.sh"],
    args = [
        "$(location :illuminator)",
        "$(location //:illuminator.yaml.example)",
        "$(location //src/plugin/test:test_source_plugin.so)",
    ],
    data = [
        ":illuminator",
        "//:illuminator.yaml.example",
        "//src/plugin/test:test_source_plugin.so",
    ],
)
