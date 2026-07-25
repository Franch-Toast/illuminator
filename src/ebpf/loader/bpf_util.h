// ============================================================================
// bpf_util — eBPF 通用工具函数集
// ============================================================================
//
// 独立的无状态工具函数，不参与任何继承链。
// 子类插件在自己的 hook 实现中按需调用。
//
// 对标 Linux 内核的 devm_request_irq()、dma_alloc_coherent() 等设备工具：
// 驱动不是通过继承获得能力，而是主动调用工具函数。
// ============================================================================

#pragma once

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"

namespace illuminator::bpf_util {

// ============================================================================
// PID/Comm 过滤
// ============================================================================

inline void WritePidFilter(int map_fd, const std::vector<uint32_t>& pids) {
    if (map_fd < 0) return;
    uint8_t val = 1;
    for (uint32_t pid : pids)
        bpf_map_update_elem(map_fd, &pid, &val, BPF_ANY);
}

inline void WriteCommFilter(int map_fd, const std::vector<std::string>& comms) {
    if (map_fd < 0) return;
    uint8_t val = 1;
    for (const auto& comm : comms) {
        char key[16] = {};
        std::memcpy(key, comm.c_str(), std::min(comm.size(), sizeof(key) - 1));
        bpf_map_update_elem(map_fd, key, &val, BPF_ANY);
    }
}

inline void ClearBpfHashMap(int map_fd) {
    if (map_fd < 0) return;
    uint32_t cur{}, next{};
    std::vector<uint32_t> keys;
    int err = bpf_map_get_next_key(map_fd, nullptr, &cur);
    while (err == 0) {
        keys.push_back(cur);
        err = bpf_map_get_next_key(map_fd, &cur, &next);
        cur = next;
    }
    for (auto k : keys)
        bpf_map_delete_elem(map_fd, &k);
}

inline void RewritePidFilter(int map_fd, const std::vector<uint32_t>& pids) {
    ClearBpfHashMap(map_fd);
    WritePidFilter(map_fd, pids);
}

inline void RewriteCommFilter(int map_fd, const std::vector<std::string>& comms) {
    if (map_fd < 0) return;
    char cur_key[16] = {}, next_key[16] = {};
    std::vector<std::string> old_comms;
    int err = bpf_map_get_next_key(map_fd, nullptr, cur_key);
    while (err == 0) {
        old_comms.emplace_back(cur_key, strnlen(cur_key, sizeof(cur_key)));
        err = bpf_map_get_next_key(map_fd, cur_key, next_key);
        std::memcpy(cur_key, next_key, 16);
    }
    for (auto& c : old_comms) {
        char k[16] = {};
        std::memcpy(k, c.c_str(), std::min(c.size(), sizeof(k) - 1));
        bpf_map_delete_elem(map_fd, k);
    }
    WriteCommFilter(map_fd, comms);
}

// ============================================================================
// PID Namespace 配置
// ============================================================================

inline void ConfigurePidNamespace(int pidns_map_fd) {
    if (pidns_map_fd < 0) return;
    struct stat st = {};
    if (::stat("/proc/self/ns/pid", &st) != 0) return;
    il_pidns_config cfg = {};
    cfg.dev = static_cast<uint64_t>(st.st_dev);
    cfg.ino = static_cast<uint64_t>(st.st_ino);
    uint32_t k = 0;
    bpf_map_update_elem(pidns_map_fd, &k, &cfg, BPF_ANY);
}

// ============================================================================
// BPF Map 简便写入
// ============================================================================

inline void WriteMapU32(int map_fd, uint32_t key, uint32_t val) {
    if (map_fd < 0) return;
    bpf_map_update_elem(map_fd, &key, &val, BPF_ANY);
}

// ============================================================================
// perf_event 管理
// ============================================================================

struct PerfConfig {
    uint32_t type = PERF_TYPE_SOFTWARE;
    uint64_t config = PERF_COUNT_SW_CPU_CLOCK;
    uint64_t sample_freq = 49;
    bool freq_mode = true;
    bool exclude_user = false;
    bool exclude_kernel = false;
};

inline long PerfEventOpenSys(struct perf_event_attr* attr, pid_t pid,
                             int cpu, int group_fd, unsigned long flags) {
#if defined(SYS_perf_event_open)
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
#elif defined(__NR_perf_event_open)
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
#else
    (void)attr; (void)pid; (void)cpu; (void)group_fd; (void)flags;
    return -1;
#endif
}

inline std::vector<int> ParseOnlineCpuIds() {
    std::vector<int> cpus;
    std::ifstream f("/sys/devices/system/cpu/online");
    if (!f) return cpus;
    std::string line;
    std::getline(f, line);
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, ',')) {
        try {
            auto dash = part.find('-');
            if (dash == std::string::npos) {
                cpus.push_back(std::stoi(part));
            } else {
                int lo = std::stoi(part.substr(0, dash));
                int hi = std::stoi(part.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) cpus.push_back(c);
            }
        } catch (...) {}
    }
    return cpus;
}

inline std::vector<int> AttachPerfEvents(int prog_fd, const PerfConfig& cfg) {
    std::vector<int> fds;
    auto cpus = ParseOnlineCpuIds();
    if (cpus.empty()) cpus.push_back(0);

    for (int cpu : cpus) {
        struct perf_event_attr attr = {};
        attr.size = sizeof(attr);
        attr.type = cfg.type;
        attr.config = cfg.config;
        attr.freq = cfg.freq_mode ? 1 : 0;
        attr.sample_freq = cfg.sample_freq;
        attr.disabled = 1;
        attr.exclude_user = cfg.exclude_user ? 1 : 0;
        attr.exclude_kernel = cfg.exclude_kernel ? 1 : 0;

        int fd = static_cast<int>(PerfEventOpenSys(&attr, -1, cpu, -1, 0));
        if (fd < 0) continue;

        if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
            close(fd);
            continue;
        }
        if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
            close(fd);
            continue;
        }
        fds.push_back(fd);
    }
    return fds;
}

inline void DetachPerfEvents(std::vector<int>& fds) {
    for (int fd : fds) {
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        close(fd);
    }
    fds.clear();
}

inline void DisablePerfEvents(const std::vector<int>& fds) {
    for (int fd : fds)
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
}

inline void EnablePerfEvents(const std::vector<int>& fds) {
    for (int fd : fds)
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
}

inline void AdjustPerfFrequency(const std::vector<int>& fds, uint64_t new_freq) {
    for (int fd : fds) {
        struct perf_event_attr attr = {};
        attr.size = sizeof(attr);
        attr.freq = 1;
        attr.sample_freq = new_freq;
        ioctl(fd, PERF_EVENT_IOC_PERIOD, &attr.sample_freq);
    }
}

}  // namespace illuminator::bpf_util
