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

// ---- CPU 子系统 ----
#include "sources/cpu/proc_stat_reader/proc_stat_reader.h"
#include "sources/cpu/ebpf_cpu_sampler/ebpf_cpu_sampler.h"
#include "sources/cpu/cpu_utilization/cpu_utilization.h"
#include "sources/cpu/process_cpu/process_cpu.h"
#include "sources/cpu/cpu_profiler/cpu_profiler.h"

// ---- 调度子系统 ----
#include "sources/sched/sched_analyzer/sched_analyzer.h"
#include "sources/sched/ebpf_sched_tracer/ebpf_sched_tracer.h"
#include "sources/sched/offcpu_profiler/offcpu_profiler.h"

// ---- I/O 子系统 ----
#include "sources/io/ebpf_io_monitor/ebpf_io_monitor.h"

// ---- 网络子系统 ----
#include "sources/net/ebpf_net_tracer/ebpf_net_tracer.h"

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
