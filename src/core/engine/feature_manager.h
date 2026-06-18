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

// FeatureState — Feature 生命周期状态机
// 状态转换路径：
//   Inactive → Starting → Active ⇄ Paused
//                ↓            ↓         ↓
//                └────────────┴─→ Stopping → Inactive
enum class FeatureState {
    kInactive,   // 未激活（初始状态，或停止后恢复到此状态）
    kStarting,   // 启动中（Pipeline 正在创建，避免重复启动）
    kActive,     // 运行中（Pipeline 正常采集并处理数据）
    kPaused,     // 暂停（Pipeline 线程仍在运行，但定时器已取消，不采集数据）
    kStopping,   // 停止中（Pipeline 正在销毁，定时器已取消）
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

// FeatureTier — Feature 资源消耗等级
// 用于前端自动启动策略和资源预算管理：
//   Tier 1 (Monitoring): 纯 procfs 读取，CPU < 0.5%，切换标签页时自动启动
//   Tier 2 (Tracing):    轻量 eBPF + procfs，CPU 1-3%，自动启动
//   Tier 3 (Profiling):  高频采样，CPU 3-10%，需手动触发（必须指定 target_pids）
enum class FeatureTier {
    kMonitoring = 1,  // Tier 1: 纯 procfs 读取，极低开销
    kTracing = 2,     // Tier 2: 轻量 eBPF + procfs，中等开销
    kProfiling = 3,   // Tier 3: 高频采样，高开销，需手动触发
};

// FeatureConfig — 静态配置（注册时设定，不会运行时改变）
struct FeatureConfig {
    std::string name;             // 内部标识名（如 "cpu_utilization"）
    std::string display_name;     // 前端显示名
    std::string category;         // 分类（如 "cpu", "memory", "io"）
    FeatureTier tier = FeatureTier::kMonitoring;  // 资源消耗等级
    PipelineConfig pipeline;      // 对应的 Pipeline 配置
};

// FeatureInfo — 运行时状态快照（ListFeatures 返回）
struct FeatureInfo {
    std::string name;
    std::string display_name;
    std::string category;
    FeatureTier tier = FeatureTier::kMonitoring;
    FeatureState state;           // 当前生命周期状态
    bool is_recording = false;    // 是否正在录制
    uint64_t batches_processed = 0;  // 已处理批次数
    uint64_t records_processed = 0;  // 已处理记录数
    uint64_t errors = 0;             // 错误计数
    uint64_t uptime_ms = 0;          // 运行时长（毫秒）
};

class FeatureManager {
public:
    // 状态变更回调类型：当 Feature 状态变化时通知外部（如 WebSocket 推送）
    using StateChangeCallback = std::function<void(const std::string& feature,
                                                    FeatureState from,
                                                    FeatureState to)>;

    // 构造：传入 PipelineController 引用，共享基础设施（TimerWheel、CollectPool、SinkPool）
    explicit FeatureManager(PipelineController& controller)
        : controller_(controller) {}

    // ---- RegisterFeature — 注册 Feature 配置（启动前调用） ----
    // 将 Feature 的静态配置注册到内部 map，初始状态为 kInactive。
    // 此方法只注册，不启动 Pipeline。
    void RegisterFeature(FeatureConfig config) {
        std::unique_lock lock(mutex_);
        auto& entry = features_[config.name];
        entry.config = std::move(config);
        entry.state = FeatureState::kInactive;
        IL_INFO("Feature registered: {}", entry.config.name);
    }

    // StartParams — 启动参数（运行时传入，可覆盖配置中的 target_pids）
    struct StartParams {
        std::vector<uint32_t> target_pids;      // 目标进程 PID 列表
        std::vector<std::string> target_comms;  // 目标进程名列表
    };

    // ---- Start — 启动 Feature ----
    // 状态转换：Inactive/Paused → Starting → Active
    // 如果已 Active 且传入了新的 target_pids，则在线重配置过滤器（不重启）
    // 安全保护：Tier 3 (Profiling) 必须指定 target_pids，防止系统级采样导致死机
    Status Start(const std::string& name, const StartParams& params = {}) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state == FeatureState::kActive) {
            if (!params.target_pids.empty()) {
                lock.unlock();
                return ReconfigureFilter(name, params);
            }
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

        // Safety: Tier 3 profiling features MUST have target_pids
        // 安全检查：Tier 3 (Profiling) 必须指定 target_pids，防止系统级采样
        if (entry.config.tier == FeatureTier::kProfiling &&
            params.target_pids.empty() &&
            entry.config.pipeline.source.config["target_pids"].AsString("").empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Profiling features require target_pids to prevent "
                                 "system-wide sampling (which may freeze the system). "
                                 "Pass target_pids in the request body.");
        }

        auto prev = entry.state;
        entry.state = FeatureState::kStarting;
        lock.unlock();
        NotifyStateChange(name, prev, FeatureState::kStarting);

        auto status = CreateAndStartPipeline(name, params);
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
        IL_INFO("Feature started: {} (target_pids={})", name, params.target_pids.size());
        return Status::Ok();
    }

    // ---- ReconfigureFilter — 在线更新过滤器（不重启 Pipeline） ----
    // 更新 Source 的 target_pids / target_comms，并清除 StreamSinkStore 旧数据。
    // 使用 shared_lock 读锁，允许并发查询但不允许并发修改。
    Status ReconfigureFilter(const std::string& name, const StartParams& params) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end() || !it->second.pipeline) {
            return Status::Error(StatusCode::kNotFound, "Feature not found or not running");
        }
        auto* source = it->second.pipeline->GetSource();
        if (!source) {
            return Status::Error(StatusCode::kInternal, "No source in pipeline");
        }
        lock.unlock();

        std::string pid_str;
        for (size_t i = 0; i < params.target_pids.size(); ++i) {
            if (i > 0) pid_str += ",";
            pid_str += std::to_string(params.target_pids[i]);
        }
        std::string comm_str;
        for (size_t i = 0; i < params.target_comms.size(); ++i) {
            if (i > 0) comm_str += ",";
            comm_str += params.target_comms[i];
        }

        ConfigValue recfg;
        recfg.Set("target_pids", pid_str);
        recfg.Set("target_comms", comm_str);
        auto st = source->Reconfigure(recfg);
        if (st.ok()) {
            StreamSinkStore::Instance().RemoveBuffer(name);
        }
        return st;
    }

    // ---- Stop — 停止 Feature ----
    // 状态转换：Active/Paused → Stopping → Inactive
    // 步骤：取消 TimerWheel 定时器 → Pipeline::Stop() → 清理 RecordingSink → 清理 StreamSink
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

    // ---- Pause — 暂停 Feature（Pipeline 线程仍在运行，但取消定时器） ----
    // 状态转换：Active → Paused
    // 取消所有 TimerWheel 定时器（Collect 和 Flush），但 Pipeline 线程保持运行。
    // 这允许快速恢复（Resume 只需重新注册定时器，无需重建 Pipeline）。
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

    // ---- Resume — 恢复 Feature（重新注册定时器） ----
    // 状态转换：Paused → Active
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

    // ---- ListFeatures — 列出所有 Feature 运行时快照 ----
    // 使用 shared_lock 读锁，允许并发读取。
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
                // 通过 RecordingSinkRegistry 查询录制状态
                auto rec_sink = RecordingSinkRegistry::Instance().Get(name);
                info.is_recording = rec_sink && rec_sink->IsRecording();
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

    // ---- SetStateChangeCallback — 设置状态变更回调（供 WebSocketManager 使用） ----
    // 当 Feature 状态变化时，通过此回调推送到 WebSocket 客户端。
    void SetStateChangeCallback(StateChangeCallback cb) {
        state_callback_ = std::move(cb);
    }

    // ---- StopAll — 停止所有活跃 Feature ----
    // 先收集所有活跃 Feature 名（读锁），再逐个 Stop（写锁）。
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
    // FeatureEntry — 内部存储结构
    // 每个注册的 Feature 对应一个 entry，包含配置、运行时状态和 Pipeline 实例。
    struct FeatureEntry {
        FeatureConfig config;
        FeatureState state = FeatureState::kInactive;
        std::unique_ptr<Pipeline> pipeline;          // Pipeline 实例（拥有所有权）
        std::chrono::steady_clock::time_point started_at;  // 启动时间戳
        std::vector<uint32_t> timer_ids;             // 已注册的 TimerWheel 定时器 ID 列表
    };

    // ---- CreateAndStartPipeline — 创建并启动 Pipeline 实例 ----
    // 从 FeatureConfig 创建完整的 Pipeline，包括：
    //   1. 通过 PluginRegistry 创建 Source/Processor/Sink 插件
    //   2. 自动注入 StreamSink（/collect API 拉取）和 WebSocketSink（实时推送）
    //   3. 自动注入 RecordingSink（录制）并注册到全局 Registry
    //   4. 注册 Pull Source 采集定时器到 TimerWheel（Push Source 不需要）
    //   5. 注册 Aggregator 刷盘定时器到 TimerWheel
    //   6. 启动 Pipeline
    Status CreateAndStartPipeline(const std::string& name,
                                   const StartParams& params = {}) {
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

        // Merge runtime params into source config (target_pids override)
        // 运行时参数合并到 Source 配置中（target_pids 覆盖配置中的默认值）
        ConfigValue source_cfg = cfg.source.config;
        if (!params.target_pids.empty()) {
            std::string pid_str;
            for (size_t i = 0; i < params.target_pids.size(); ++i) {
                if (i > 0) pid_str += ",";
                pid_str += std::to_string(params.target_pids[i]);
            }
            source_cfg.Set("target_pids", pid_str);
        }
        if (!params.target_comms.empty()) {
            std::string comm_str;
            for (size_t i = 0; i < params.target_comms.size(); ++i) {
                if (i > 0) comm_str += ",";
                comm_str += params.target_comms[i];
            }
            source_cfg.Set("target_comms", comm_str);
        }

        auto init_status = source->Init(source_cfg);
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

        // 自动注入 StreamSink（/collect API 拉取）和 WebSocketSink（实时推送）和 RecordingSink（录制）
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

        // 清除 StreamSinkStore 中的旧数据，避免重启后返回过期采样
        StreamSinkStore::Instance().RemoveBuffer(name);

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
