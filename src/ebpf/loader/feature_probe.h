#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include "core/common/logging.h"

namespace illuminator {

struct KernelFeatures {
    bool has_btf = false;
    bool has_ringbuf = false;
    bool has_bpf_tracing = false;
    int major = 0;
    int minor = 0;
    int patch = 0;
};

inline KernelFeatures ProbeKernelFeatures() {
    KernelFeatures f;

    // Check BTF availability
    std::ifstream btf("/sys/kernel/btf/vmlinux");
    f.has_btf = btf.good();

    // Parse kernel version
    std::ifstream version("/proc/version");
    if (version.good()) {
        std::string line;
        std::getline(version, line);
        // "Linux version X.Y.Z ..."
        if (sscanf(line.c_str(), "Linux version %d.%d.%d",
                   &f.major, &f.minor, &f.patch) < 2) {
            f.major = f.minor = f.patch = 0;
        }
    }

    // Ring buffer requires kernel 5.8+
    f.has_ringbuf = (f.major > 5) || (f.major == 5 && f.minor >= 8);

    // BPF tracing requires kernel 4.14+
    f.has_bpf_tracing = (f.major > 4) || (f.major == 4 && f.minor >= 14);

    IL_INFO("Kernel: %d.%d.%d BTF=%s RingBuf=%s BPFTracing=%s",
            f.major, f.minor, f.patch,
            f.has_btf ? "yes" : "no",
            f.has_ringbuf ? "yes" : "no",
            f.has_bpf_tracing ? "yes" : "no");

    return f;
}

}  // namespace illuminator
