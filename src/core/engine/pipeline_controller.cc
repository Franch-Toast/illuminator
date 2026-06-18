// ============================================================================
// PipelineController v3 实现 — 全局管道生命周期管理
// ============================================================================
// BuildFromConfig: 从 GlobalConfig 构建所有 Pipeline + 共享基础设施
// StartAll:       启动所有 Pipeline + 注册定时器 + 启动 TimerWheel
// StopAll:        停止定时器 → 停止采集池 → 停止管道 → 等待写入完成
// ============================================================================

#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"
#include "storage/storage_backend.h"

namespace illuminator {

// ====================================================================
// BuildFromConfig — 从 GlobalConfig 构建所有 Pipeline
// ====================================================================
// 遍历配置中的每个 Pipeline 配置项，通过 PluginRegistry 创建插件实例，
// 组装 Source → Processor[] → Aggregator(可选) → Sink[] 管线。
// 最后初始化共享的 SinkPool 和 CollectPool。
//
// 参数解析：
//   channel.size: "small"=1024, "medium"=4096(默认), "large"=16384
//   channel.drop_policy: "drop_newest"(默认) 或 "drop_oldest"
//   channel.backpressure_high/low: 反压水位线比例
Status PipelineController::BuildFromConfig(const GlobalConfig& config) {
    auto& registry = PluginRegistry::Instance();

    // 将字符串容量映射为实际数值
    auto resolve_capacity = [](const std::string& size) -> size_t {
        if (size == "small")  return 1024;
        if (size == "large")  return 16384;
        return 4096;  // "medium" or default
    };

    for (auto& pc : config.pipelines) {
        // 解析丢包策略
        DropPolicy dp = DropPolicy::kDropNewest;
        if (config.engine.channel.drop_policy == "drop_oldest") {
            dp = DropPolicy::kDropOldest;
        }
        // 创建 Pipeline 实例（配置 channel 容量、丢包策略、水位线）
        auto pipeline = std::make_unique<Pipeline>(
            pc.name,
            resolve_capacity(config.engine.channel.size),
            dp,
            config.engine.channel.backpressure_high,
            config.engine.channel.backpressure_low);

        // 创建 Source 插件
        auto source = registry.CreateSource(pc.source.type);
        if (!source) {
            return Status::Error(StatusCode::kNotFound,
                "Source plugin not found: " + pc.source.type +
                " (pipeline: " + pc.name + ")");
        }
        auto status = source->Init(pc.source.config);
        if (!status.ok()) return status;
        pipeline->SetSource(std::move(source));

        // 创建 Processor 链（处理器按配置顺序串行执行）
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

        // 创建 Aggregator（可选，聚合器用于定时批量输出）
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

        // 创建 Sink 列表（数据出口，至少一个）
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

    // 初始化共享基础设施
    InitSinkPool(config.engine.sink_pool_threads);
    InitCollectPool(config.engine.collect_pool_threads);

    return Status::Ok();
}

// ====================================================================
// StartAll — 启动所有 Pipeline + 注册定时器 + 启动 TimerWheel
// ====================================================================
// 启动流程（严格按顺序）：
//   1. 逐个启动 Pipeline（Source → Processor → Aggregator → Sink → ProcessThread）
//   2. 含 BPF 探针的管道间添加 100ms 延迟（避免多个 BPF 程序同时挂载到内核热路径）
//   3. 为 Pull Source 注册定时采集事件到 TimerWheel
//   4. 为有 Aggregator 的管道注册定时刷盘事件到 TimerWheel
//   5. 注册定期指标同步（每 10 秒）
//   6. 注册定期数据清理（每 60 秒，保留最近 30 分钟数据）
//   7. 启动 TimerWheel 调度线程
//
// 任何 Pipeline 启动失败 → 立即调用 StopAll 清理已启动的管道 → 返回错误
Status PipelineController::StartAll() {
    // Phase 1: 逐个启动 Pipeline
    for (auto& pipeline : pipelines_) {
        auto status = pipeline->Start();
        if (!status.ok()) {
            IL_ERROR("Failed to start pipeline '{}': {}",
                     pipeline->name(), status.message());
            StopAll();  // 启动失败时清理已启动的管道
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

    // Phase 2: 注册 Pull Source 定时采集事件
    // Register Pull-mode sources as TimerWheel collect events
    for (auto& p : pipelines_) {
        auto* src = p->GetSource();
        if (src && !src->IsPushMode()) {
            auto interval = std::chrono::milliseconds(src->IntervalMs());
            Pipeline* pipeline_ptr = p.get();
            SourcePlugin* source_ptr = src;

            // 定时器回调：提交到 CollectPool 异步执行采集
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

    // Phase 3: 注册 Aggregator 定时刷盘事件
    // Register Aggregator flush events as TimerWheel sentinel injections
    for (auto& p : pipelines_) {
        if (p->HasAggregator()) {
            auto interval = std::chrono::milliseconds(p->FlushIntervalMs());
            Pipeline* pipeline_ptr = p.get();

            // 定时器回调：直接注入 FlushSentinel（不需要 CollectPool）
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

    // Phase 4: 注册定期指标同步（每 10 秒）
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

    // Phase 5: 注册定期数据清理（每 60 秒，保留最近 30 分钟）
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

    // Phase 6: 启动 TimerWheel 调度线程
    timer_.Start();

    IL_INFO("All {} pipelines started (v3: TimerWheel + CollectPool + SinkPool)",
            pipelines_.size());
    return Status::Ok();
}

// ====================================================================
// StopAll — 停止所有 Pipeline + 共享基础设施
// ====================================================================
// 停止顺序（与启动顺序相反，保证优雅停机）：
//   Phase 1: TimerWheel::Stop() — 停止所有定时器，不再触发 Collect/Flush
//   Phase 2: CollectPool 销毁 — 等待正在执行的 Collect 任务完成
//   Phase 3: 逐个 Pipeline::Stop() — ProcessThread 排空 channel 后退出
//   Phase 4: SinkPool 销毁（析构函数中） — 等待所有 Write 任务完成
Status PipelineController::StopAll() {
    // Phase 1: 停止 TimerWheel — 不再触发新的 Collect/Flush 事件
    timer_.Stop();

    // Phase 2: 停止 CollectPool — 等待正在执行中的 Collect 完成
    collect_pool_.reset();

    // Phase 3: 停止每个 Pipeline（ProcessThread 排空 channel 后退出）
    for (auto& pipeline : pipelines_) {
        pipeline->Stop();
    }

    // Phase 4: SinkPool 最后销毁（确保最后的写入完成）
    // sink_pool_ destroyed in destructor

    IL_INFO("All pipelines stopped");
    return Status::Ok();
}

// ====================================================================
// GetPipeline — 按名称查找 Pipeline
// ====================================================================
// 线性遍历 pipelines_ 列表，返回匹配的 Pipeline 指针。
// 未找到时返回 nullptr。
Pipeline* PipelineController::GetPipeline(const std::string& name) {
    for (auto& p : pipelines_) {
        if (p->name() == name) return p.get();
    }
    return nullptr;
}

}  // namespace illuminator
