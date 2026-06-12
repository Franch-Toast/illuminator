// ============================================================================
// FeatureManager — 功能级生命周期管理（按需启停 + 实时流 + 录制）
// ============================================================================
//
// Feature 是面向用户的最小可操作单元（1 Feature = 1 Pipeline = 1 前端面板）
//
// 状态模型:
//   Inactive → Starting → Active → Paused → Active
//      ↑                      |                |
//      +────── Stopping ←─────+────────────────+
//
// 与 PipelineController 的关系:
//   PipelineController 管理共享基础设施（TimerWheel、CollectPool、SinkPool）
//   FeatureManager 管理各个 Feature 的按需启停、使用共享基础设施
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"
#include "sinks/recording_sink/recording_sink.h"
#include "sinks/stream_sink/stream_sink.h"
#include "sinks/websocket_sink/websocket_sink.h"

namespace illuminator {

class WebSocketManager;

enum class FeatureState {
    kInactive,
    kStarting,
    kActive,
    kPaused,
    kStopping,
};

inline const char* FeatureStateToString(FeatureState s) {
    switch (s) {
        case FeatureState::kInactive:  return "inactive";
        case FeatureState::kStarting:  return "starting";
        case FeatureState::kActive:    return "active";
        case FeatureState::kPaused:    return "paused";
        case FeatureState::kStopping:  return "stopping";
    }
    return "unknown";
}

enum class FeatureTier {
    kMonitoring = 1,  // Tier 1: procfs reading, < 0.5% CPU, auto-start on tab enter
    kTracing = 2,     // Tier 2: lightweight eBPF + procfs, 1-3% CPU, auto-start
    kProfiling = 3,   // Tier 3: high-frequency sampling, 3-10% CPU, manual trigger
};

struct FeatureConfig {
    std::string name;
    std::string display_name;
    std::string category;
    FeatureTier tier = FeatureTier::kMonitoring;
    PipelineConfig pipeline;
    uint32_t window_sec = 60;
};

struct FeatureInfo {
    std::string name;
    std::string display_name;
    std::string category;
    FeatureTier tier = FeatureTier::kMonitoring;
    FeatureState state;
    bool is_recording = false;
    uint64_t batches_processed = 0;
    uint64_t records_processed = 0;
    uint64_t errors = 0;
    uint64_t uptime_ms = 0;
};

class FeatureManager {
public:
    using StateChangeCallback = std::function<void(const std::string& feature,
                                                    FeatureState from,
                                                    FeatureState to)>;

    explicit FeatureManager(PipelineController& controller)
        : controller_(controller) {}

    void RegisterFeature(FeatureConfig config) {
        std::unique_lock lock(mutex_);
        auto& entry = features_[config.name];
        entry.config = std::move(config);
        entry.state = FeatureState::kInactive;
        IL_INFO("Feature registered: {}", entry.config.name);
    }

    Status Start(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state == FeatureState::kActive) {
            return Status::Ok();
        }
        if (entry.state != FeatureState::kInactive &&
            entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature cannot be started in state: " +
                                 std::string(FeatureStateToString(entry.state)));
        }

        if (entry.state == FeatureState::kPaused) {
            return ResumeInternal(entry);
        }

        auto prev = entry.state;
        entry.state = FeatureState::kStarting;
        lock.unlock();
        NotifyStateChange(name, prev, FeatureState::kStarting);

        auto status = CreateAndStartPipeline(name);
        lock.lock();

        if (!status.ok()) {
            entry.state = FeatureState::kInactive;
            NotifyStateChange(name, FeatureState::kStarting, FeatureState::kInactive);
            return status;
        }

        entry.state = FeatureState::kActive;
        entry.started_at = std::chrono::steady_clock::now();
        lock.unlock();
        NotifyStateChange(name, FeatureState::kStarting, FeatureState::kActive);
        IL_INFO("Feature started: {}", name);
        return Status::Ok();
    }

    Status Stop(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state == FeatureState::kInactive) {
            return Status::Ok();
        }
        if (entry.state != FeatureState::kActive &&
            entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature cannot be stopped in state: " +
                                 std::string(FeatureStateToString(entry.state)));
        }

        auto prev = entry.state;
        entry.state = FeatureState::kStopping;
        lock.unlock();
        NotifyStateChange(name, prev, FeatureState::kStopping);

        StopAndDestroyPipeline(name);

        lock.lock();
        entry.state = FeatureState::kInactive;
        entry.pipeline.reset();
        lock.unlock();
        NotifyStateChange(name, FeatureState::kStopping, FeatureState::kInactive);
        IL_INFO("Feature stopped: {}", name);
        return Status::Ok();
    }

    Status Pause(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state != FeatureState::kActive) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature must be active to pause");
        }

        PausePipeline(entry);
        entry.state = FeatureState::kPaused;
        lock.unlock();
        NotifyStateChange(name, FeatureState::kActive, FeatureState::kPaused);
        return Status::Ok();
    }

    Status Resume(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature must be paused to resume");
        }
        return ResumeInternal(entry);
    }

    std::vector<FeatureInfo> ListFeatures() const {
        std::shared_lock lock(mutex_);
        std::vector<FeatureInfo> result;
        result.reserve(features_.size());
        for (const auto& [name, entry] : features_) {
            FeatureInfo info;
            info.name = name;
            info.display_name = entry.config.display_name;
            info.category = entry.config.category;
            info.tier = entry.config.tier;
            info.state = entry.state;
            if (entry.pipeline) {
                info.batches_processed = entry.pipeline->BatchesProcessed();
                info.records_processed = entry.pipeline->RecordsProcessed();
                info.errors = entry.pipeline->ErrorCount();
                if (entry.state == FeatureState::kActive ||
                    entry.state == FeatureState::kPaused) {
                    auto elapsed = std::chrono::steady_clock::now() - entry.started_at;
                    info.uptime_ms = std::chrono::duration_cast<
                        std::chrono::milliseconds>(elapsed).count();
                }
            }
            result.push_back(std::move(info));
        }
        return result;
    }

    FeatureState GetState(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) return FeatureState::kInactive;
        return it->second.state;
    }

    Pipeline* GetPipeline(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end() || !it->second.pipeline) return nullptr;
        return it->second.pipeline.get();
    }

    void SetStateChangeCallback(StateChangeCallback cb) {
        state_callback_ = std::move(cb);
    }

    void StopAll() {
        std::vector<std::string> active_features;
        {
            std::shared_lock lock(mutex_);
            for (const auto& [name, entry] : features_) {
                if (entry.state == FeatureState::kActive ||
                    entry.state == FeatureState::kPaused) {
                    active_features.push_back(name);
                }
            }
        }
        for (const auto& name : active_features) {
            Stop(name);
        }
    }

private:
    struct FeatureEntry {
        FeatureConfig config;
        FeatureState state = FeatureState::kInactive;
        std::unique_ptr<Pipeline> pipeline;
        std::chrono::steady_clock::time_point started_at;
        std::vector<uint32_t> timer_ids;
        bool paused = false;
    };

    Status CreateAndStartPipeline(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found");
        }
        auto& entry = it->second;
        const auto& cfg = entry.config.pipeline;
        lock.unlock();

        auto& registry = PluginRegistry::Instance();

        auto pipeline = std::make_unique<Pipeline>(name);

        auto source = registry.CreateSource(cfg.source.type);
        if (!source) {
            return Status::Error(StatusCode::kInternal,
                                 "Failed to create source: " + cfg.source.type);
        }
        auto init_status = source->Init(cfg.source.config);
        if (!init_status.ok()) return init_status;

        pipeline->SetSource(std::move(source));

        for (const auto& proc_cfg : cfg.processors) {
            auto proc = registry.CreateProcessor(proc_cfg.type);
            if (proc) {
                proc->Init(proc_cfg.config);
                pipeline->AddProcessor(std::move(proc));
            }
        }

        for (const auto& sink_cfg : cfg.sinks) {
            auto sink = registry.CreateSink(sink_cfg.type);
            if (sink) {
                sink->Init(sink_cfg.config);
                pipeline->AddSink(std::move(sink));
            }
        }

        // 自动注入 StreamSink 用于 /collect API 数据拉取
        auto stream_sink = std::make_unique<StreamSink>();
        stream_sink->SetFeatureName(name);
        pipeline->AddSink(std::move(stream_sink));

        // 注入 WebSocketSink 用于 WS 实时推送（pipeline_key = feature name）
        auto ws_sink = std::make_unique<WebSocketSink>();
        ConfigValue ws_cfg;
        ws_cfg.Set("pipeline_key", name);
        ws_cfg.Set("max_buffer_size", int64_t{10});
        ws_sink->Init(ws_cfg);
        pipeline->AddSink(std::move(ws_sink));

        // 注入 RecordingSink 并注册到全局 Registry 供 API 访问
        auto rec_sink = std::make_unique<RecordingSink>();
        rec_sink->SetFeatureName(name);
        auto* rec_ptr = rec_sink.get();
        pipeline->AddSink(std::move(rec_sink));
        RecordingSinkRegistry::Instance().Register(name, rec_ptr);

        pipeline->SetSinkPool(controller_.GetSinkPool());

        auto start_status = pipeline->Start();
        if (!start_status.ok()) return start_status;

        auto& timer = controller_.GetTimerWheel();
        auto* src_ptr = pipeline->GetSource();
        auto* pipe_ptr = pipeline.get();

        std::vector<uint32_t> timer_ids;

        if (!src_ptr->IsPushMode()) {
            uint32_t interval_ms = src_ptr->IntervalMs();
            if (interval_ms == 0) interval_ms = 1000;
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(interval_ms),
                [this, pipe_ptr, src_ptr, name]() {
                    auto* pool = controller_.GetCollectPool();
                    if (!pool) return;
                    pool->Submit([pipe_ptr, src_ptr]() {
                        auto result = src_ptr->Collect();
                        if (result.ok() && *result && !(*result)->Empty()) {
                            pipe_ptr->Enqueue(std::move(*result));
                        }
                    });
                }
            );
            timer_ids.push_back(tid);
        }

        if (pipe_ptr->HasAggregator()) {
            uint32_t flush_ms = pipe_ptr->FlushIntervalMs();
            if (flush_ms == 0) flush_ms = 3000;
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(flush_ms),
                [pipe_ptr]() { pipe_ptr->InjectFlush(); }
            );
            timer_ids.push_back(tid);
        }

        std::unique_lock wlock(mutex_);
        auto wit = features_.find(name);
        if (wit != features_.end()) {
            wit->second.pipeline = std::move(pipeline);
            wit->second.timer_ids = std::move(timer_ids);
        }
        return Status::Ok();
    }

    void StopAndDestroyPipeline(const std::string& name) {
        // 先注销 RecordingSink 引用（避免悬挂指针）
        RecordingSinkRegistry::Instance().Unregister(name);

        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) return;
        auto& entry = it->second;

        auto& timer = controller_.GetTimerWheel();
        for (auto tid : entry.timer_ids) {
            timer.Cancel(tid);
        }
        entry.timer_ids.clear();

        if (entry.pipeline) {
            lock.unlock();
            entry.pipeline->Stop();
            lock.lock();
        }
    }

    void PausePipeline(FeatureEntry& entry) {
        auto& timer = controller_.GetTimerWheel();
        for (auto tid : entry.timer_ids) {
            timer.Cancel(tid);
        }
        entry.timer_ids.clear();
        entry.paused = true;
    }

    Status ResumeInternal(FeatureEntry& entry) {
        if (!entry.pipeline || !entry.pipeline->IsRunning()) {
            return Status::Error(StatusCode::kInternal,
                                 "Pipeline not running for resume");
        }

        auto& timer = controller_.GetTimerWheel();
        auto* src_ptr = entry.pipeline->GetSource();
        auto* pipe_ptr = entry.pipeline.get();
        const auto& name = entry.config.name;

        if (!src_ptr->IsPushMode()) {
            uint32_t interval_ms = src_ptr->IntervalMs();
            if (interval_ms == 0) interval_ms = 1000;
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(interval_ms),
                [this, pipe_ptr, src_ptr, name]() {
                    auto* pool = controller_.GetCollectPool();
                    if (!pool) return;
                    pool->Submit([pipe_ptr, src_ptr]() {
                        auto result = src_ptr->Collect();
                        if (result.ok() && *result && !(*result)->Empty()) {
                            pipe_ptr->Enqueue(std::move(*result));
                        }
                    });
                }
            );
            entry.timer_ids.push_back(tid);
        }

        entry.state = FeatureState::kActive;
        entry.paused = false;
        NotifyStateChange(name, FeatureState::kPaused, FeatureState::kActive);
        return Status::Ok();
    }

    void NotifyStateChange(const std::string& name, FeatureState from, FeatureState to) {
        if (state_callback_) {
            state_callback_(name, from, to);
        }
    }

    PipelineController& controller_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FeatureEntry> features_;
    StateChangeCallback state_callback_;
};

}  // namespace illuminator
