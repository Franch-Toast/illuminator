// ============================================================================
// PipelineController v3 implementation
// ============================================================================

#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"
#include "storage/storage_backend.h"

namespace illuminator {

Status PipelineController::BuildFromConfig(const GlobalConfig& config) {
    auto& registry = PluginRegistry::Instance();

    auto resolve_capacity = [](const std::string& size) -> size_t {
        if (size == "small")  return 1024;
        if (size == "large")  return 16384;
        return 4096;  // "medium" or default
    };

    for (auto& pc : config.pipelines) {
        DropPolicy dp = DropPolicy::kDropNewest;
        if (config.engine.channel.drop_policy == "drop_oldest") {
            dp = DropPolicy::kDropOldest;
        }
        auto pipeline = std::make_unique<Pipeline>(
            pc.name,
            resolve_capacity(config.engine.channel.size),
            dp,
            config.engine.channel.backpressure_high,
            config.engine.channel.backpressure_low);

        auto source = registry.CreateSource(pc.source.type);
        if (!source) {
            return Status::Error(StatusCode::kNotFound,
                "Source plugin not found: " + pc.source.type +
                " (pipeline: " + pc.name + ")");
        }
        auto status = source->Init(pc.source.config);
        if (!status.ok()) return status;
        pipeline->SetSource(std::move(source));

        for (auto& proc_cfg : pc.processors) {
            auto proc = registry.CreateProcessor(proc_cfg.type);
            if (!proc) {
                return Status::Error(StatusCode::kNotFound,
                    "Processor plugin not found: " + proc_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = proc->Init(proc_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddProcessor(std::move(proc));
        }

        if (pc.aggregator.has_value()) {
            auto agg = registry.CreateAggregator(pc.aggregator->type);
            if (!agg) {
                return Status::Error(StatusCode::kNotFound,
                    "Aggregator plugin not found: " + pc.aggregator->type +
                    " (pipeline: " + pc.name + ")");
            }
            status = agg->Init(pc.aggregator->config);
            if (!status.ok()) return status;
            pipeline->SetAggregator(std::move(agg));
        }

        for (auto& sink_cfg : pc.sinks) {
            auto sink = registry.CreateSink(sink_cfg.type);
            if (!sink) {
                return Status::Error(StatusCode::kNotFound,
                    "Sink plugin not found: " + sink_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = sink->Init(sink_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddSink(std::move(sink));
        }

        IL_INFO("Built pipeline: {}", pc.name);
        pipelines_.push_back(std::move(pipeline));
    }

    InitSinkPool(config.engine.sink_pool_threads);
    InitCollectPool(config.engine.collect_pool_threads);

    return Status::Ok();
}

Status PipelineController::StartAll() {
    for (auto& pipeline : pipelines_) {
        auto status = pipeline->Start();
        if (!status.ok()) {
            IL_ERROR("Failed to start pipeline '{}': {}",
                     pipeline->name(), status.message());
            StopAll();
            return status;
        }
        // 含 BPF 探针的管道在启动间添加延迟，避免多个 BPF 程序
        // 同时挂载到调度器热路径导致内核瞬时过载
        if (pipeline->GetSource() &&
            (pipeline->GetSource()->IsPushMode() ||
             pipeline->GetSource()->HasBpfProbe())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Register Pull-mode sources as TimerWheel collect events
    for (auto& p : pipelines_) {
        auto* src = p->GetSource();
        if (src && !src->IsPushMode()) {
            auto interval = std::chrono::milliseconds(src->IntervalMs());
            Pipeline* pipeline_ptr = p.get();
            SourcePlugin* source_ptr = src;

            timer_.AddRepeating(interval,
                [this, pipeline_ptr, source_ptr] {
                    if (!pipeline_ptr->IsRunning()) return;
                    collect_pool_->Submit(
                        [pipeline_ptr, source_ptr] {
                            auto result = source_ptr->Collect();
                            if (result.ok() && *result && !(*result)->Empty()) {
                                pipeline_ptr->Enqueue(std::move(*result));
                            }
                        }
                    );
                }
            );
            IL_INFO("Registered Pull source for pipeline '{}' (interval={}ms)",
                     pipeline_ptr->name(), src->IntervalMs());
        }
    }

    // Register Aggregator flush events as TimerWheel sentinel injections
    for (auto& p : pipelines_) {
        if (p->HasAggregator()) {
            auto interval = std::chrono::milliseconds(p->FlushIntervalMs());
            Pipeline* pipeline_ptr = p.get();

            timer_.AddRepeating(interval,
                [pipeline_ptr] {
                    if (pipeline_ptr->IsRunning()) {
                        pipeline_ptr->InjectFlush();
                    }
                }
            );
            IL_INFO("Registered Aggregator flush for pipeline '{}' (interval={}ms)",
                     pipeline_ptr->name(), p->FlushIntervalMs());
        }
    }

    // Register periodic metrics sync
    timer_.AddRepeating(std::chrono::seconds(10),
        [this] {
            if (collect_pool_) {
                collect_pool_->Submit([this] {
                    auto& m = InternalMetrics::Instance();
                    m.SetGauge("timer_wheel_fires_total",
                               static_cast<double>(timer_.FiresTotal()));
                    m.SetGauge("timer_wheel_timers_active",
                               static_cast<double>(timer_.ActiveTimers()));
                    if (collect_pool_) {
                        m.SetGauge("collect_pool_pending_tasks",
                                   static_cast<double>(collect_pool_->PendingTasks()));
                    }
                    if (sink_pool_) {
                        m.SetGauge("sink_pool_pending_tasks",
                                   static_cast<double>(sink_pool_->PendingTasks()));
                    }
                });
            }
        }
    );

    // Register periodic data pruning (every 60s, keep last 30 minutes)
    if (storage_backend_) {
        static constexpr uint64_t kRetentionNs = 30ULL * 60 * 1000000000ULL;
        timer_.AddRepeating(std::chrono::seconds(60),
            [this] {
                if (sink_pool_) {
                    sink_pool_->Submit([this] {
                        storage_backend_->Prune(kRetentionNs);
                    });
                }
            }
        );
        IL_INFO("Registered storage data pruning (retention=30min, interval=60s)");
    }

    timer_.Start();

    IL_INFO("All {} pipelines started (v3: TimerWheel + CollectPool + SinkPool)",
            pipelines_.size());
    return Status::Ok();
}

Status PipelineController::StopAll() {
    // Phase 1: Stop TimerWheel — no more Collect/Flush events
    timer_.Stop();

    // Phase 2: Stop CollectPool — wait for in-flight Collects
    collect_pool_.reset();

    // Phase 3: Stop each pipeline (ProcessThread drains channel)
    for (auto& pipeline : pipelines_) {
        pipeline->Stop();
    }

    // Phase 4: SinkPool destroyed last (ensure last writes complete)
    // sink_pool_ destroyed in destructor

    IL_INFO("All pipelines stopped");
    return Status::Ok();
}

Pipeline* PipelineController::GetPipeline(const std::string& name) {
    for (auto& p : pipelines_) {
        if (p->name() == name) return p.get();
    }
    return nullptr;
}

}  // namespace illuminator
