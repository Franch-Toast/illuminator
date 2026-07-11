"""Bazel rules for compiling BPF C sources to .bpf.o objects and skeleton headers."""

_BPF_INCLUDES = "-Isrc/ebpf/include -isystem /usr/include"

_BPF_HDRS = [
    "//src/ebpf:include/vmlinux.h",
    "//src/ebpf:include/common.bpf.h",
    "//src/ebpf:include/event_types.h",
]

def bpf_probe(name, src, arch = "x86"):
    """Compile a single BPF probe .bpf.c → .bpf.o

    Uses $$BPF_CLANG from --action_env (default: clang).
    Requires libbpf-dev installed on the host (provides bpf/bpf_helpers.h).
    """
    copts = "-g -O2 -target bpf -D__TARGET_ARCH_" + arch
    native.genrule(
        name = name,
        srcs = [src] + _BPF_HDRS,
        outs = [name + ".bpf.o"],
        cmd = "$${{BPF_CLANG:-clang}} {} {} -c $(location {}) -o $@".format(
            copts, _BPF_INCLUDES, src),
        visibility = ["//visibility:public"],
    )

def bpf_skeleton(name, src, arch = "x86"):
    """Compile a BPF probe and generate a type-safe skeleton header.

    Produces three targets:
      {name}_bpf       — raw .bpf.o (via clang)
      {name}_skel_gen  — .skel.h (via bpftool gen skeleton)
      {name}_skel      — cc_library wrapping the skeleton header

    Requires bpftool on the host ($$BPF_BPFTOOL or default /usr/sbin/bpftool).
    """
    copts = "-g -O2 -target bpf -D__TARGET_ARCH_" + arch

    # 1. Compile .bpf.c → .bpf.o
    native.genrule(
        name = name + "_bpf",
        srcs = [src] + _BPF_HDRS,
        outs = [name + ".bpf.o"],
        cmd = "$${{BPF_CLANG:-clang}} {} {} -c $(location {}) -o $@".format(
            copts, _BPF_INCLUDES, src),
        visibility = ["//visibility:public"],
    )

    # 2. Generate skeleton header: .bpf.o → .skel.h
    native.genrule(
        name = name + "_skel_gen",
        srcs = [":" + name + "_bpf"],
        outs = [name + ".skel.h"],
        cmd = """
            BPFTOOL=$$(command -v bpftool || echo /usr/sbin/bpftool)
            $$BPFTOOL gen skeleton $(location :{name}_bpf) > $@
        """.format(name = name),
        visibility = ["//visibility:public"],
    )

    # 3. cc_library wrapping the skeleton header
    native.cc_library(
        name = name + "_skel",
        hdrs = [":" + name + "_skel_gen"],
        includes = ["."],
        visibility = ["//visibility:public"],
    )
