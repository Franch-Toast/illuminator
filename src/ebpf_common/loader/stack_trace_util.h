#pragma once

#include "ebpf_common/include/bpf_compat.h"
#include <cstdint>
#include <vector>

#include "core/common/data_batch.h"
#include "ebpf_common/include/event_types.h"

namespace illuminator {

inline bool IsCanonicalAddress(uint64_t addr) {
    return addr <= 0x00007FFFFFFFFFFFULL || addr >= 0xFFFF800000000000ULL;
}

inline std::vector<StackFrame> LookupBpfStackTrace(int stacks_fd, int32_t stack_id,
                                                    size_t max_depth = MAX_STACK_DEPTH) {
    std::vector<StackFrame> frames;
    if (stack_id <= 0 || stacks_fd < 0) return frames;

    uint64_t ips[MAX_STACK_DEPTH] = {};
    if (bpf_map_lookup_elem(stacks_fd, &stack_id, ips) != 0) return frames;

    for (size_t i = 0; i < max_depth && i < MAX_STACK_DEPTH && ips[i] != 0; ++i) {
        if (!IsCanonicalAddress(ips[i])) continue;
        StackFrame f;
        f.address = ips[i];
        frames.push_back(f);
    }
    return frames;
}

}  // namespace illuminator
