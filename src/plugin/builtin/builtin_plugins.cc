#include "plugin/builtin/builtin_plugins.h"

// Sources
#include "sources/proc_stat_reader/proc_stat_reader.h"
#include "sources/ebpf_cpu_sampler/ebpf_cpu_sampler.h"
#include "sources/ebpf_net_tracer/ebpf_net_tracer.h"
#include "sources/ebpf_io_monitor/ebpf_io_monitor.h"
#include "sources/ebpf_sched_tracer/ebpf_sched_tracer.h"

// Processors
#include "processors/passthrough/passthrough_processor.h"
#include "processors/filter/filter_processor.h"

// Sinks
#include "sinks/console_output/console_sink.h"
#include "sinks/file_export/file_export_sink.h"
#include "sinks/local_storage/local_storage_sink.h"
#include "sinks/pprof_export/pprof_export_sink.h"
#include "sinks/prometheus_exposition/prometheus_sink.h"
#include "sinks/otlp_export/otlp_export_sink.h"

// Storage backends (force-link)
#include "storage/sqlite_backend/sqlite_backend.h"

namespace illuminator {

void RegisterBuiltinPlugins() {
    // Static registration already happened via NS_REGISTER_* macros.
}

}  // namespace illuminator
