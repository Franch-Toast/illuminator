// =============================================================================
// SinkFanout — 数据分发器（组合 Sink）
// =============================================================================
// 将一份 DataBatch 复制分发到多个子 Sink。
// 用途：
//   - SseSink（实时流）+ RecordingSink（录制落盘）同时消费
//   - 支持动态添加/移除子 Sink（热插拔录制功能）
//
// 性能考虑：
//   - DataBatch 使用 shared_ptr，多个 Sink 共享同一份数据（零拷贝）
//   - 子 Sink 的 Write 在同一线程内串行执行
//   - 单个子 Sink 失败不影响其他 Sink
// =============================================================================

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "plugin/api/sink_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class SinkFanout : public SinkPlugin {
public:
    const char* Name() const override { return "fanout"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& /*config*/) override {
        return Status::Ok();
    }

    // 将 batch 分发到所有子 Sink（共享 ptr，零拷贝）
    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        std::lock_guard<std::mutex> lk(mu_);
        for (auto& sink : sinks_) {
            auto st = sink->Write(batch);
            if (!st.ok()) {
                IL_WARN("SinkFanout: child sink '{}' write failed: {}",
                        sink->Name(), st.message());
            }
        }
        return Status::Ok();
    }

    // 动态添加子 Sink
    void AddSink(std::shared_ptr<SinkPlugin> sink) {
        std::lock_guard<std::mutex> lk(mu_);
        sinks_.push_back(std::move(sink));
    }

    // 按名称移除子 Sink
    bool RemoveSink(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = std::remove_if(sinks_.begin(), sinks_.end(),
                                 [&name](const auto& s) {
                                     return s->Name() == name;
                                 });
        if (it == sinks_.end()) return false;
        sinks_.erase(it, sinks_.end());
        return true;
    }

    size_t SinkCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return sinks_.size();
    }

    // 获取子 Sink 列表（用于诊断）
    std::vector<std::string> SinkNames() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> names;
        names.reserve(sinks_.size());
        for (const auto& s : sinks_)
            names.emplace_back(s->Name());
        return names;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<SinkPlugin>> sinks_;
};

IL_REGISTER_SINK("fanout", SinkFanout);

}  // namespace illuminator
