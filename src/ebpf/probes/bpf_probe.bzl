"""Bazel rule for compiling BPF C sources to .bpf.o objects."""

_BPF_INCLUDES = "-Isrc/ebpf/include"

_BPF_HDRS = [
    "//src/ebpf:include/vmlinux.h",
    "//src/ebpf:include/common.bpf.h",
    "//src/ebpf:include/event_types.h",
]

def bpf_probe(name, src, arch = "x86"):
    """Compile a single BPF probe .bpf.c → .bpf.o

    Uses $$BPF_CLANG from --action_env (default: clang).
    """
    copts = "-g -O2 -target bpf -D__TARGET_ARCH_" + arch
    native.genrule(
        name = name,
        srcs = [src] + _BPF_HDRS,
        outs = [name + ".bpf.o"],
        cmd = "$${BPF_CLANG:-clang} {} {} -c $(location {}) -o $@".format(
            copts, _BPF_INCLUDES, src),
        visibility = ["//visibility:public"],
    )
