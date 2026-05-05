#pragma once

// ============================================================================
// 文件：illuminator/src/ebpf/loader/feature_probe.h
// ============================================================================
//
// 【作用】
// 内核 eBPF 特性探测模块。在 Illuminator 启动时自动检测当前运行内核的
// eBPF 相关能力，返回 KernelFeatures 结构体供上层决策使用。
//
// 【工作原理】
// 1. BTF（BPF Type Format）探测：
//    检查 /sys/kernel/btf/vmlinux 文件是否存在。
//    BTF 是 BPF CO-RE（Compile Once, Run Everywhere）的基础设施，
//    允许编译后的 BPF 程序在不同内核版本上安全运行。
//    内核 5.2+ 通常支持 BTF（需要 CONFIG_DEBUG_INFO_BTF=y）。
//
// 2. 内核版本解析：
//    读取 /proc/version 文件，解析 "Linux version X.Y.Z" 格式的版本号。
//    例如 "5.15.0-91-generic" → major=5, minor=15, patch=0。
//    版本号用于功能降级判断（如低版本内核使用 perf buffer 而非 ring buffer）。
//
// 3. Ring Buffer 判定：
//    要求内核 ≥ 5.8。Linux 5.8 引入了 BPF ring buffer map 类型，
//    相比旧的 perf buffer map 有更低的延迟和更好的 CPU 亲和性。
//    不满足版本要求时，应回退到 perf buffer 方案。
//
// 4. BPF Tracing 判定：
//    要求内核 ≥ 4.14。Linux 4.14 是 eBPF 功能的里程碑版本，
//    引入了 BPF raw tracepoint、perf event array 等核心追踪特性。
//
// 【设计约束】
// - 必须内联实现（inline），因为定义在头文件中，避免链接问题
// - 探测失败不应导致程序崩溃：缺失 BTF 时仅关闭 CO-RE 相关功能
// - 探测结果通过 IL_INFO 日志输出，便于问题排查
// ============================================================================

#include <cstdio>
#include <fstream>
#include <string>
#include "core/common/logging.h"

namespace illuminator {

// ============================================================================
// KernelFeatures：内核 eBPF 特性探测结果
//
// 汇总当前运行内核的各项 eBPF 能力，以布尔标志 + 版本号组合呈现。
// 上层代码（PipelineController 或 BpfProgramManager）根据这些标志
// 决定启用哪些 eBPF 探针以及使用何种 map 类型。
// ============================================================================
struct KernelFeatures {
    bool has_btf = false;          // 是否支持 BTF（BPF Type Format）：CO-RE 编译的关键
    bool has_ringbuf = false;      // 是否支持 BPF ring buffer（内核 ≥ 5.8）
    bool has_bpf_tracing = false;  // 是否支持 BPF raw tracing（内核 ≥ 4.14）
    int major = 0;                 // 内核主版本号（如 5.15 中的 5）
    int minor = 0;                 // 内核次版本号（如 5.15 中的 15）
    int patch = 0;                 // 内核修订号（如 5.15.91 中的 91）
};

// ============================================================================
// ProbeKernelFeatures：探测当前内核的 eBPF 特性
//
// 功能：一次性检测当前运行内核的所有 eBPF 相关能力。
// 返回的 KernelFeatures 结构体可被缓存复用，无需重复探测。
//
// 探测步骤：
// 1. BTF 检测 → 打开 /sys/kernel/btf/vmlinux，检查文件是否可读
// 2. 版本解析 → 读取 /proc/version，sscanf 解析语义版本号
// 3. Ring buffer 判定 → 根据大/小版本号判断（≥ 5.8）
// 4. BPF tracing 判定 → 根据大/小版本号判断（≥ 4.14）
//
// 返回值：填充完毕的 KernelFeatures 结构体
// 异常处理：任何文件读取失败 → 对应字段保持默认 false/0，不影响其他探测
// ============================================================================
inline KernelFeatures ProbeKernelFeatures() {
    KernelFeatures f;

    // -------------------------------------------------------------------
    // 探测 1：检测 BTF（BPF Type Format）支持
    // BTF 文件位于 /sys/kernel/btf/vmlinux，由内核模块加载时生成
    // 如果文件存在且可读，说明内核支持 BTF 且 BTF 数据可用
    // -------------------------------------------------------------------
    std::ifstream btf("/sys/kernel/btf/vmlinux");
    f.has_btf = btf.good();

    // -------------------------------------------------------------------
    // 探测 2：解析内核版本号
    // /proc/version 文件包含类似 "Linux version 5.15.0-91-generic ..." 的内容
    // 通过 sscanf 提取三段数字：major.minor.patch
    // 解析失败时所有版本字段保持 0
    // -------------------------------------------------------------------
    std::ifstream version("/proc/version");
    if (version.good()) {
        std::string line;
        std::getline(version, line);
        // 格式示例："Linux version 5.15.0-91-generic (buildd@...) (gcc ...)"
        if (sscanf(line.c_str(), "Linux version %d.%d.%d",
                   &f.major, &f.minor, &f.patch) < 2) {
            // 至少需要读取 major 和 minor，失败则清零
            f.major = f.minor = f.patch = 0;
        }
    }

    // -------------------------------------------------------------------
    // 探测 3：Ring buffer 可用性判定
    // BPF ring buffer（BPF_MAP_TYPE_RINGBUF）在 Linux 5.8 引入
    // 较旧内核只能使用 perf_event_array（BPF_MAP_TYPE_PERF_EVENT_ARRAY）
    // Ring buffer 优势：CPU 亲和性更好、延迟更低、无需 perf 子系统依赖
    // -------------------------------------------------------------------
    f.has_ringbuf = (f.major > 5) || (f.major == 5 && f.minor >= 8);

    // -------------------------------------------------------------------
    // 探测 4：BPF tracing 可用性判定
    // BPF 原始 tracepoint（BPF_PROG_TYPE_RAW_TRACEPOINT）在 Linux 4.14 引入
    // 此特性是 CPU 采样、调度追踪等核心功能的先决条件
    // -------------------------------------------------------------------
    f.has_bpf_tracing = (f.major > 4) || (f.major == 4 && f.minor >= 14);

    // -------------------------------------------------------------------
    // 输出探测结果以帮助运维排查环境兼容性问题
    // -------------------------------------------------------------------
    IL_INFO("Kernel: %d.%d.%d BTF=%s RingBuf=%s BPFTracing=%s",
            f.major, f.minor, f.patch,
            f.has_btf ? "yes" : "no",
            f.has_ringbuf ? "yes" : "no",
            f.has_bpf_tracing ? "yes" : "no");

    return f;
}

}  // namespace illuminator
