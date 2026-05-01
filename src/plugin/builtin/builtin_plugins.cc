#include "plugin/builtin/builtin_plugins.h"

// Legacy Sources (kept for backward compatibility)
#include "sources/proc_stat_reader/proc_stat_reader.h"
#include "sources/cpu_sys_monitor/cpu_sys_monitor.h"
#include "sources/process_cpu_monitor/process_cpu_monitor.h"
#include "sources/ebpf_cpu_sampler/ebpf_cpu_sampler.h"
#include "sources/ebpf_net_tracer/ebpf_net_tracer.h"
#include "sources/ebpf_io_monitor/ebpf_io_monitor.h"
#include "sources/ebpf_sched_tracer/ebpf_sched_tracer.h"

// New CPU monitoring Sources
#include "sources/cpu_utilization/cpu_utilization.h"
#include "sources/process_cpu/process_cpu.h"
#include "sources/cpu_profiler/cpu_profiler.h"
#include "sources/sched_analyzer/sched_analyzer.h"
#include "sources/offcpu_profiler/offcpu_profiler.h"

// Processors
#include "processors/passthrough/passthrough_processor.h"
#include "processors/filter/filter_processor.h"
#include "processors/stack_symbolizer/stack_symbolizer.h"
#include "processors/stack_merger/stack_merger.h"

// Aggregators
#include "aggregators/cpu_stats_aggregator/cpu_stats_aggregator.h"

// Sinks
#include "sinks/console_output/console_sink.h"
#include "sinks/file_export/file_export_sink.h"
#include "sinks/local_storage/local_storage_sink.h"
#include "sinks/pprof_export/pprof_export_sink.h"
#include "sinks/prometheus_exposition/prometheus_sink.h"
#include "sinks/otlp_export/otlp_export_sink.h"
#include "sinks/websocket_sink/websocket_sink.h"

// Storage backends (force-link)
#include "storage/sqlite_backend/sqlite_backend.h"

namespace illuminator {

void RegisterBuiltinPlugins() {
    // Static registration already happened via IL_REGISTER_* macros.
}

}  // namespace illuminator
