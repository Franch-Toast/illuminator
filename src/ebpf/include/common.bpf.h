#ifndef __ILLUMINATOR_COMMON_BPF_H
#define __ILLUMINATOR_COMMON_BPF_H

// BPF-side only header: includes kernel types and BPF helpers.
// For userspace code, include event_types.h instead.

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "event_types.h"

#define IL_RINGBUF_SIZE (256 * 1024)

#endif /* __ILLUMINATOR_COMMON_BPF_H */
