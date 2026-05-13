// ============================================================================
// Illuminator 内置插件强链接清单
// ============================================================================
//
// 本文件通过 #include 引用所有内置插件的头文件，确保它们的
// 静态初始化器（IL_REGISTER_* 宏）被链接到最终二进制中。
//
// 如果没有这个文件，C++ 链接器可能会因为"没有代码显式引用这些符号"
// 而丢弃插件定义所在的翻译单元，导致 PluginRegistry 在运行时找不到这些插件。
//
// 插件清单（共 26+ 个插件）：
// ============================
// Sources（数据源 12个）：
//   proc_stat_reader, cpu_sys_monitor, process_cpu_monitor,
//   ebpf_cpu_sampler, ebpf_net_tracer, ebpf_io_monitor, ebpf_sched_tracer,
//   cpu_utilization, process_cpu, cpu_profiler,
//   sched_analyzer, offcpu_profiler（+ cpu_sys_stats, proc_cpu_monitor）
//
// Processors（4个）：
//   passthrough, filter, stack_symbolizer, stack_merger
//
// Aggregators（1个）：
//   cpu_stats_aggregator
//
// Sinks（7个）：
//   console_output, file_export, local_storage, pprof_export,
//   prometheus_exposition, otlp_export, websocket_sink
//
// Storage（1个）：
//   sqlite
//
// RegisterBuiltinPlugins() 函数体为空 — 所有注册工作在静态初始化阶段完成。
// ============================================================================

#include "plugin/builtin/builtin_plugins.h"

// ---- 遗留数据源（保持向后兼容） ----
#include "sources/proc_stat_reader/proc_stat_reader.h"          // 读取 /proc/stat、/proc/meminfo、/proc/loadavg
#include "sources/cpu_sys_monitor/cpu_sys_monitor.h"            // 系统级 CPU 指标（利用率、上下文切换、负载）
#include "sources/process_cpu_monitor/process_cpu_monitor.h"    // 按进程/线程的 CPU 利用率（htop 风格）
#include "sources/ebpf_cpu_sampler/ebpf_cpu_sampler.h"          // eBPF CPU 采样器（流式模式）
#include "sources/ebpf_net_tracer/ebpf_net_tracer.h"            // eBPF TCP 连接追踪
#include "sources/ebpf_io_monitor/ebpf_io_monitor.h"            // eBPF 块设备 I/O 延迟监控
#include "sources/ebpf_sched_tracer/ebpf_sched_tracer.h"        // eBPF 调度器事件追踪（唤醒/切换）

// ---- 新版 CPU 监控数据源 ----
#include "sources/cpu_utilization/cpu_utilization.h"            // 统一 CPU 利用率源（EMA 平滑、多维指标）
#include "sources/process_cpu/process_cpu.h"                    // 进程 CPU 监控（正则过滤、Top-N、线程详情）
#include "sources/cpu_profiler/cpu_profiler.h"                  // CPU 性能剖析（eBPF perf_event + 堆栈聚合）
#include "sources/sched_analyzer/sched_analyzer.h"              // 调度分析器（运行队列延迟、迁移追踪）
#include "sources/offcpu_profiler/offcpu_profiler.h"            // Off-CPU 性能剖析（等待时间分析）

// ---- 补充数据源 (通过其他 BUILD 目标注册) ----
#include "sources/cpu_sys_stats/cpu_sys_stats.h"
#include "sources/proc_cpu_monitor/proc_cpu_monitor.h"

// ---- 数据处理器 ----
#include "processors/passthrough/passthrough_processor.h"       // 透传处理器（无操作，测试用）
#include "processors/filter/filter_processor.h"                  // 过滤处理器（按标签值丢弃记录）
#include "processors/stack_symbolizer/stack_symbolizer.h"       // 堆栈符号化（地址 → 函数名，含 ELF 解析）
#include "processors/stack_merger/stack_merger.h"               // 堆栈合并器（相同调用栈计数累加）

// ---- 聚合器 ----
#include "aggregators/cpu_stats_aggregator/cpu_stats_aggregator.h"  // CPU 统计聚合器

// ---- 数据出口 ----
#include "sinks/console_output/console_sink.h"                  // 控制台输出（文本/JSON 格式）
#include "sinks/file_export/file_export_sink.h"                 // 文件导出（JSONL 格式）
#include "sinks/local_storage/local_storage_sink.h"             // 本地存储（委托给 StorageBackend）
#include "sinks/pprof_export/pprof_export_sink.h"               // pprof 格式导出（折叠栈兼容 FlameGraph）
#include "sinks/prometheus_exposition/prometheus_sink.h"        // Prometheus 指标暴露
#include "sinks/otlp_export/otlp_export_sink.h"                 // OTLP 导出（JSON over HTTP）
#include "sinks/websocket_sink/websocket_sink.h"                // WebSocket 推送

// ---- 存储后端（强制链接） ----
#include "storage/sqlite_backend/sqlite_backend.h"              // SQLite 持久化存储

namespace illuminator {

// 函数体为空 — 所有插件的真正注册在静态初始化阶段通过 IL_REGISTER_* 宏自动完成。
// 调用此函数的作用是确保上面的 #include 引用的翻译单元不被链接器丢弃。
void RegisterBuiltinPlugins() {
    // 静态注册已通过 IL_REGISTER_* 宏自动完成。
}

}  // namespace illuminator
