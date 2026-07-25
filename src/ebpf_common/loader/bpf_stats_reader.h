// ============================================================================
// BPF Meta Stats Reader — 读取 eBPF 侧自观测 PERCPU_ARRAY
// ============================================================================
//
// 所有声明了 meta_stats map 的 BPF 程序共享相同的读取逻辑：
//   1. 通过 bpf_num_possible_cpus() 获取可能的 CPU 数量
//   2. 为每个 key（0..3）读取每个 CPU 的 u64 值
//   3. 对所有 CPU 求和，得到累计计数
//
// 如果 fd < 0（旧版 BPF 程序未导出 meta_stats），直接返回零值。
// ============================================================================

#pragma once

#include <cstdint>
#include <unistd.h>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "plugin/api/source_plugin.h"

namespace illuminator {

inline MetaStats ReadBpfMetaStats(int fd) {
    MetaStats stats{};
    if (fd < 0)
        return stats;

    int nprocs = libbpf_num_possible_cpus();
    if (nprocs <= 0)
        nprocs = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (nprocs <= 0)
        nprocs = 1;

    std::vector<uint64_t> values(static_cast<size_t>(nprocs), 0);
    for (uint32_t key = 0; key < 4; ++key) {
        std::fill(values.begin(), values.end(), 0);
        if (bpf_map_lookup_elem(fd, &key, values.data()) != 0)
            continue;

        uint64_t sum = 0;
        for (int i = 0; i < nprocs; ++i)
            sum += values[i];

        switch (key) {
            case 0: stats.total_events = sum; break;
            case 1: stats.buffer_full = sum; break;
            case 2: stats.dropped = sum; break;
            case 3: stats.filtered = sum; break;
        }
    }
    return stats;
}

}  // namespace illuminator
