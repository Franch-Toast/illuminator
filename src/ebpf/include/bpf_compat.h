// Compatibility shim: older kernel UAPI headers lack bpf_link_type.
// Include this header instead of <bpf/libbpf.h> directly.
#pragma once

#ifndef BPF_LINK_TYPE_UNSPEC
enum bpf_link_type { BPF_LINK_TYPE_UNSPEC = 0 };
#define BPF_LINK_TYPE_UNSPEC BPF_LINK_TYPE_UNSPEC
#endif

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
