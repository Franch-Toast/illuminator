// Compatibility shim: include this instead of <bpf/libbpf.h> directly.
// Handles the bpf_link_type enum availability across kernel versions.
//
// - Kernel >= 5.7 (Ubuntu 22.04+): linux/bpf.h defines full bpf_link_type
// - Kernel < 5.7 (Ubuntu 20.04):   linux/bpf.h lacks it, we provide a shim
//
// Detection: BPF_F_REPLACE macro was introduced in the same release (5.7).
#pragma once

#include <linux/bpf.h>

#ifndef BPF_F_REPLACE
enum bpf_link_type { BPF_LINK_TYPE_UNSPEC = 0 };
#endif

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
