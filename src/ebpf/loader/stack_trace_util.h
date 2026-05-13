#pragma once

#include <bpf/libbpf.h>
#include <cstdint>
#include <vector>

#include "core/engine/data_batch.h"
#include "ebpf/include/event_types.h"

namespace illuminator {

inline std::vector<StackFrame> LookupBpfStackTrace(int stacks_fd, int32_t stack_id,
                                                    size_t max_depth = MAX_STACK_DEPTH) {
    std::vector<StackFrame> frames;
    if (stack_id <= 0 || stacks_fd < 0) return frames;

    uint64_t ips[MAX_STACK_DEPTH] = {};
    if (bpf_map_lookup_elem(stacks_fd, &stack_id, ips) != 0) return frames;

    for (size_t i = 0; i < max_depth && i < MAX_STACK_DEPTH && ips[i] != 0; ++i) {
        StackFrame f;
        f.address = ips[i];
        frames.push_back(f);
    }
    return frames;
}

}  // namespace illuminator
