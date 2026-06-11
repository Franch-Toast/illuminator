// =============================================================================
// StreamSink — Feature 实时数据流 Sink
// =============================================================================
// 为每个 Feature 提供独立的数据缓冲区，支持：
//   1. HTTP API 拉取（/api/v1/features/:name/collect）
//   2. WebSocket 实时推送
//   3. 未来的录制功能扩展（通过 SinkFanout 组合）
//
// 与 WebSocketSink 的区别：
//   - StreamSink 面向 Feature 维度，而非全局 pipeline_key
//   - 支持增量拉取（带 cursor），避免重复消费
//   - 内置淘汰策略和内存上限
// =============================================================================

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// StreamBuffer — 单个 Feature 的环形数据缓冲区
class StreamBuffer {
public:
    explicit StreamBuffer(size_t max_batches = 60)
        : max_batches_(max_batches) {}

    void Push(DataBatchPtr batch) {
        std::lock_guard<std::mutex> lk(mu_);
        batches_.push_back(std::move(batch));
        ++seq_;
        while (batches_.size() > max_batches_)
            batches_.pop_front();
    }

    // 获取最新一条
    DataBatchPtr Latest() const {
        std::lock_guard<std::mutex> lk(mu_);
        return batches_.empty() ? nullptr : batches_.back();
    }

    // 增量拉取：返回 cursor 之后的所有新 batch，更新 cursor
    std::vector<DataBatchPtr> PollSince(uint64_t& cursor) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<DataBatchPtr> out;
        uint64_t available = seq_;
        if (cursor >= available) return out;

        size_t skip = 0;
        if (available - cursor > batches_.size()) {
            skip = 0;
            cursor = available - batches_.size();
        } else {
            skip = batches_.size() - (available - cursor);
        }

        for (size_t i = skip; i < batches_.size(); ++i)
            out.push_back(batches_[i]);
        cursor = available;
        return out;
    }

    // 获取最近 N 条
    std::vector<DataBatchPtr> Recent(size_t n) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<DataBatchPtr> out;
        size_t start = batches_.size() > n ? batches_.size() - n : 0;
        for (size_t i = start; i < batches_.size(); ++i)
            out.push_back(batches_[i]);
        return out;
    }

    uint64_t Sequence() const { return seq_.load(); }
    size_t Size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return batches_.size();
    }

private:
    mutable std::mutex mu_;
    std::deque<DataBatchPtr> batches_;
    std::atomic<uint64_t> seq_{0};
    size_t max_batches_;
};

// StreamSinkStore — 全局流数据存储（按 Feature 名索引）
class StreamSinkStore {
public:
    static StreamSinkStore& Instance() {
        static StreamSinkStore inst;
        return inst;
    }

    StreamBuffer& GetBuffer(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = buffers_.find(feature_name);
        if (it == buffers_.end()) {
            auto [inserted, _] = buffers_.emplace(
                feature_name, std::make_unique<StreamBuffer>());
            return *inserted->second;
        }
        return *it->second;
    }

    void RemoveBuffer(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        buffers_.erase(feature_name);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<StreamBuffer>> buffers_;
};

// StreamSink — 将 DataBatch 推送到 StreamBuffer
class StreamSink : public SinkPlugin {
public:
    const char* Name() const override { return "stream_sink"; }
    const char* Version() const override { return "0.2.0"; }

    Status Init(const ConfigValue& config) override {
        feature_name_ = config["feature_name"].AsString("default");
        max_buffer_ = static_cast<size_t>(config["max_buffer"].AsInt(60));
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        StreamSinkStore::Instance().GetBuffer(feature_name_).Push(
            std::move(batch));
        return Status::Ok();
    }

    void SetFeatureName(const std::string& name) { feature_name_ = name; }

private:
    std::string feature_name_ = "default";
    size_t max_buffer_ = 60;
};

IL_REGISTER_SINK("stream_sink", StreamSink);

}  // namespace illuminator
