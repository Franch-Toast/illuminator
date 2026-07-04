// ============================================================================
// FeatureDriver — Feature 驱动基类（Linux Driver Model 风格）
// ============================================================================
//
// 每个 Feature 是一个自包含的 Driver，封装完整的 Pipeline 生命周期。
// 替代旧 FeatureManager 的中心化管理方式，实现去中心化、可组合的 Feature 模型。
//
// Driver 职责：
//   1. 声明 Feature 元数据（name, tier, category, JSON Schema）
//   2. 构建自己的 Pipeline（Source + Processor + Sink 链）
//   3. 管理 Feature 的生命周期状态机
//   4. 响应运行时重配置请求
//
// 生命周期状态机：
//   Inactive → Probe() → Active ⇄ Paused → Remove() → Inactive
//
// 子类实现示例：
//   class CpuUtilizationDriver : public FeatureDriver {
//       const char* Name() override { return "cpu_utilization"; }
//       Pipeline* BuildPipeline(InfrastructureManager& infra) override { ... }
//   };
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"
#include "plugin/sinks/recording_sink/recording_sink.h"

namespace illuminator {

enum class DriverState {
    kInactive,
    kActive,
    kPaused,
};

inline const char* DriverStateToString(DriverState s) {
    switch (s) {
        case DriverState::kInactive: return "inactive";
        case DriverState::kActive:   return "active";
        case DriverState::kPaused:   return "paused";
    }
    return "unknown";
}

enum class DriverTier {
    kMonitoring = 1,
    kTracing = 2,
    kProfiling = 3,
};

enum class DataModelType {
    kTimeSeries,
    kProfile,
    kTrace,
    kLog,
};

enum class ParamType {
    kString,
    kInteger,
    kIntegerRange,
    kPidList,
    kEnum,
    kBoolean,
    kNumber,
};

struct ParamDeclaration {
    std::string key;
    std::string display_name;
    ParamType type = ParamType::kString;
    bool required = false;
    std::string default_value;
    std::string description;
    int min_value = 0;
    int max_value = 0;
    std::vector<std::string> choices;
};

struct FeatureDescriptor {
    std::string name;
    std::string display_name;
    std::string description;
    std::string category;
    std::string version;
    DriverTier tier = DriverTier::kMonitoring;
    DataModelType model = DataModelType::kTimeSeries;

    bool supports_pull = true;
    bool supports_push = false;
    bool supports_pause = true;
    bool supports_configure = false;
    bool has_bpf_probe = false;
    bool session_required = false;

    std::vector<ParamDeclaration> params;
};

struct FeatureStats {
    uint64_t batches_processed = 0;
    uint64_t records_processed = 0;
    uint64_t errors = 0;
    uint64_t uptime_ms = 0;
};

struct DriverInfo {
    std::string name;
    std::string display_name;
    std::string category;
    DriverTier tier = DriverTier::kMonitoring;
    DriverState state = DriverState::kInactive;
    uint64_t batches_processed = 0;
    uint64_t records_processed = 0;
    uint64_t uptime_ms = 0;
};

class FeatureDriver {
public:
    virtual ~FeatureDriver() { Remove(); }

    // ---- 标识与元数据 ----
    virtual const char* Name() const = 0;
    virtual const char* DisplayName() const = 0;
    virtual const char* Version() const { return "1.0.0"; }
    virtual const char* Category() const = 0;
    virtual DriverTier Tier() const = 0;

    virtual FeatureDescriptor Describe() const {
        FeatureDescriptor desc;
        desc.name = Name();
        desc.display_name = DisplayName();
        desc.category = Category();
        desc.version = Version();
        desc.tier = Tier();
        desc.supports_pause = true;
        desc.supports_configure = true;
        return desc;
    }

    // ---- 配置接口 (前端自动发现) ----
    virtual std::string ConfigSchema() const { return R"({"type":"object","properties":{}})"; }
    virtual std::string GetConfig() const { return "{}"; }
    virtual Status SetConfig(const std::string& /*json_config*/) {
        return Status::Error(StatusCode::kUnimplemented, "SetConfig not supported");
    }

    // ---- 统计接口 ----
    virtual FeatureStats GetStats() const {
        FeatureStats stats;
        if (pipeline_) {
            stats.batches_processed = pipeline_->BatchesProcessed();
            stats.records_processed = pipeline_->RecordsProcessed();
        }
        if (state_ != DriverState::kInactive) {
            auto now = std::chrono::steady_clock::now();
            stats.uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time_).count();
        }
        return stats;
    }

    // Probe — 构建 Pipeline 并启动 Feature
    Status Probe() {
        if (state_ != DriverState::kInactive) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 std::string(Name()) + " already active");
        }

        auto& infra = InfrastructureManager::Instance();
        if (!infra.IsStarted()) {
            return Status::Error(StatusCode::kUnavailable,
                                 "InfrastructureManager not started");
        }

        pipeline_ = BuildPipeline(infra);
        if (!pipeline_) {
            return Status::Error(StatusCode::kInternal,
                                 std::string(Name()) + " BuildPipeline returned null");
        }

        // Attach RecordingSink (on-demand file I/O, only writes when activated)
        auto rec_sink = std::make_unique<RecordingSink>();
        ConfigValue rec_cfg;
        rec_cfg.Set("feature_name", std::string(Name()));
        rec_cfg.Set("output_dir", std::string("/tmp/illuminator_data"));
        rec_sink->Init(rec_cfg);
        recording_sink_ = rec_sink.get();
        RecordingSinkRegistry::Instance().Register(Name(), recording_sink_);
        pipeline_->AddSink(std::move(rec_sink));

        pipeline_->SetSinkPool(infra.GetSinkPool());
        auto status = pipeline_->Start();
        if (!status.ok()) {
            RecordingSinkRegistry::Instance().Unregister(Name());
            recording_sink_ = nullptr;
            pipeline_.reset();
            return status;
        }

        RegisterTimers(infra);

        state_ = DriverState::kActive;
        start_time_ = std::chrono::steady_clock::now();
        IL_INFO("FeatureDriver '{}' probed successfully", Name());
        return Status::Ok();
    }

    // Remove — 停止并销毁 Pipeline
    Status Remove() {
        if (state_ == DriverState::kInactive) return Status::Ok();

        UnregisterTimers(InfrastructureManager::Instance());

        if (recording_sink_) {
            RecordingSinkRegistry::Instance().Unregister(Name());
            recording_sink_ = nullptr;
        }

        if (pipeline_) {
            pipeline_->Stop();
            pipeline_.reset();
        }

        state_ = DriverState::kInactive;
        IL_INFO("FeatureDriver '{}' removed", Name());
        return Status::Ok();
    }

    // Pause — 暂停采集（取消定时器，保留 Pipeline 线程）
    Status Pause() {
        if (state_ != DriverState::kActive) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 std::string(Name()) + " not active");
        }
        UnregisterTimers(InfrastructureManager::Instance());
        state_ = DriverState::kPaused;
        IL_INFO("FeatureDriver '{}' paused", Name());
        return Status::Ok();
    }

    // Resume — 恢复采集（重新注册定时器）
    Status Resume() {
        if (state_ != DriverState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 std::string(Name()) + " not paused");
        }
        RegisterTimers(InfrastructureManager::Instance());
        state_ = DriverState::kActive;
        IL_INFO("FeatureDriver '{}' resumed", Name());
        return Status::Ok();
    }

    // Reconfigure — 运行时动态更新 Feature 参数
    virtual Status Reconfigure(const ConfigValue& params) {
        return Status::Error(StatusCode::kUnimplemented, "reconfigure not supported");
    }

    DriverState State() const { return state_; }
    Pipeline* GetPipeline() { return pipeline_.get(); }

    DriverInfo Info() const {
        DriverInfo info;
        info.name = Name();
        info.display_name = DisplayName();
        info.category = Category();
        info.tier = Tier();
        info.state = state_;
        if (pipeline_) {
            info.batches_processed = pipeline_->BatchesProcessed();
            info.records_processed = pipeline_->RecordsProcessed();
        }
        if (state_ != DriverState::kInactive) {
            auto now = std::chrono::steady_clock::now();
            info.uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time_).count();
        }
        return info;
    }

protected:
    // 子类必须实现：构建自己的 Pipeline
    virtual std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) = 0;

    // 子类可选覆写：注册/取消定时器
    virtual void RegisterTimers(InfrastructureManager& infra) {
        if (!pipeline_ || !pipeline_->GetSource()) return;
        auto* src = pipeline_->GetSource();
        if (!src->IsPushMode()) {
            collect_timer_id_ = infra.GetTimerWheel().AddRepeating(
                std::chrono::milliseconds(src->IntervalMs()),
                [this, &infra] {
                    if (state_ != DriverState::kActive) return;
                    infra.GetCollectPool()->Submit([this] {
                        if (!pipeline_ || !pipeline_->GetSource()) return 0;
                        auto result = pipeline_->GetSource()->Collect();
                        if (result.ok()) {
                            pipeline_->Enqueue(std::move(result.value()));
                        }
                        return 0;
                    });
                });
        }
        if (pipeline_->HasAggregator() && pipeline_->FlushIntervalMs() > 0) {
            flush_timer_id_ = infra.GetTimerWheel().AddRepeating(
                std::chrono::milliseconds(pipeline_->FlushIntervalMs()),
                [this] { pipeline_->InjectFlush(); });
        }
    }

    virtual void UnregisterTimers(InfrastructureManager& infra) {
        if (collect_timer_id_ != 0) {
            infra.GetTimerWheel().Cancel(collect_timer_id_);
            collect_timer_id_ = 0;
        }
        if (flush_timer_id_ != 0) {
            infra.GetTimerWheel().Cancel(flush_timer_id_);
            flush_timer_id_ = 0;
        }
    }

    DriverState state_ = DriverState::kInactive;
    std::unique_ptr<Pipeline> pipeline_;
    RecordingSink* recording_sink_ = nullptr;
    std::chrono::steady_clock::time_point start_time_;
    uint64_t collect_timer_id_ = 0;
    uint64_t flush_timer_id_ = 0;
};

}  // namespace illuminator
