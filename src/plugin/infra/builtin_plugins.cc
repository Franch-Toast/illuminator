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
// RegisterBuiltinPlugins() 函数体为空 — 所有注册工作在静态初始化阶段完成。
// 实际插件清单以下面的 #include 指令为准。
// ============================================================================

#include "plugin/infra/builtin_plugins.h"

// ---- CPU 子系统 ----
#include "plugin/features/cpu/cpu_utilization/proc_stat_reader.h"
#include "plugin/features/cpu/cpu_utilization/cpu_utilization_source.h"
#include "plugin/features/cpu/process_cpu/process_cpu_source.h"
#include "plugin/features/cpu/cpu_profiler/cpu_profiler_source.h"

// ---- 调度子系统 ----
#include "plugin/features/sched/sched_analyzer/sched_analyzer_source.h"
#include "plugin/features/sched/offcpu_profiler/offcpu_profiler_source.h"

// ---- I/O 子系统 ----
#include "plugin/features/io/io_monitor/io_monitor_source.h"

// ---- 网络子系统 ----
#include "plugin/features/net/net_tracer/net_tracer_source.h"

// ---- 数据处理器 ----
#include "plugin/processors/passthrough/passthrough_processor.h"       // 透传处理器（无操作，测试用）
#include "plugin/processors/filter/filter_processor.h"                  // 过滤处理器（按标签值丢弃记录）
#include "plugin/processors/stack_symbolizer/stack_symbolizer.h"       // 堆栈符号化（地址 → 函数名，含 ELF 解析）
#include "plugin/processors/stack_merger/stack_merger.h"               // 堆栈合并器（相同调用栈计数累加）

// ---- 聚合器 ----
// （当前无内置聚合器）

// ---- 数据出口 ----
#include "plugin/sinks/console_output/console_sink.h"                  // 控制台输出（文本/JSON 格式）
#include "plugin/sinks/file_export/file_export_sink.h"                 // 文件导出（JSONL 格式）
#include "plugin/sinks/local_storage/local_storage_sink.h"             // 本地存储（委托给 StorageBackend）
#include "plugin/sinks/pprof_export/pprof_export_sink.h"               // pprof 格式导出（折叠栈兼容 FlameGraph）
#include "plugin/sinks/prometheus_exposition/prometheus_sink.h"        // Prometheus 指标暴露

// ---- 存储后端（强制链接） ----
#include "server/storage/sqlite_backend/sqlite_backend.h"              // SQLite 持久化存储

namespace illuminator {

// 函数体为空 — 所有插件的真正注册在静态初始化阶段通过 IL_REGISTER_* 宏自动完成。
// 调用此函数的作用是确保上面的 #include 引用的翻译单元不被链接器丢弃。
void RegisterBuiltinPlugins() {
    // 静态注册已通过 IL_REGISTER_* 宏自动完成。
}

}  // namespace illuminator
