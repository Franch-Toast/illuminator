// =============================================================================
// 文件：websocket_sink.h
// 模块：Illuminator 数据出口 - WebSocket 数据推送
// 描述：
//   将数据批次推送到内存缓冲区，供外部 WebSocket 服务拉取。
//   使用单例 WebSocketSinkStore 作为跨组件数据共享层。
//   支持按流水线键（pipeline_key）隔离数据，缓冲区大小可调。
// =============================================================================

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// WebSocketSinkStore: WebSocket 数据缓冲区存储（单例模式）
// 作为 Sink 和 WebSocket 服务之间的桥梁，以 pipeline_key 为维度
// 维护有限大小的 DataBatch 双端队列。
// 线程安全。
class WebSocketSinkStore {
public:
    // 获取全局唯一实例（懒初始化单例）
    static WebSocketSinkStore& Instance() {
        static WebSocketSinkStore inst;
        return inst;
    }

    // 向指定流水线的缓冲区推入一个 DataBatch
    // 若队列长度超过 max_keep，自动丢弃最旧的数据。
    // 参数:
    //   pipeline_key - 流水线标识符（用于数据隔离）
    //   batch        - 待推入的 DataBatch
    //   max_keep     - 最大保留条数
    void PushBatch(const std::string& pipeline_key, DataBatchPtr batch,
                   size_t max_keep) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& dq = buffers_[pipeline_key];
        dq.push_back(batch);
        while (dq.size() > max_keep)
            dq.pop_front();
    }

    // 获取指定流水线的最新一条 DataBatch
    // 参数:
    //   pipeline_key - 流水线标识符
    // 返回:
    //   最新的 DataBatch 指针，无数据时返回 nullptr
    DataBatchPtr Latest(const std::string& pipeline_key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = buffers_.find(pipeline_key);
        if (it == buffers_.end() || it->second.empty())
            return nullptr;
        return it->second.back();
    }

    // 获取指定流水线的最近 N 条 DataBatch 快照
    // 参数:
    //   pipeline_key - 流水线标识符
    //   max_n        - 最多返回条数
    // 返回:
    //   最近 max_n 条 DataBatch 的 vector
    std::vector<DataBatchPtr> SnapshotRecent(const std::string& pipeline_key,
                                             size_t max_n) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<DataBatchPtr> out;
        auto it = buffers_.find(pipeline_key);
        if (it == buffers_.end())
            return out;
        const auto& dq = it->second;
        size_t start = dq.size() > max_n ? dq.size() - max_n : 0;
        for (size_t i = start; i < dq.size(); ++i)
            out.push_back(dq[i]);
        return out;
    }

private:
    mutable std::mutex mu_;                                          // 保护缓冲区访问的互斥锁
    std::unordered_map<std::string, std::deque<DataBatchPtr>> buffers_; // 按 pipeline_key 索引的 DataBatch 队列
};

// WebSocketSink: WebSocket 推送 Sink
// 将数据批次推送到 WebSocketSinkStore 中，供 WebSocket 服务端轮询分发。
// 配置参数:
//   max_buffer_size - 每个流水线最多缓存的 DataBatch 数量，默认 100
//   pipeline_key    - 流水线标识符，默认 "default"
class WebSocketSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "websocket_sink"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析缓冲区大小和流水线键
    // 参数:
    //   config - 配置项，包含 max_buffer_size 和 pipeline_key
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        max_buffer_size_ =
            static_cast<size_t>(config["max_buffer_size"].AsInt(100));
        pipeline_key_ = config["pipeline_key"].AsString("default");
        return Status::Ok();
    }

    // 将 DataBatch 推送到 WebSocketSinkStore
    // 参数:
    //   batch - 待推送的数据批次
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!batch)
            return Status::Ok();
        WebSocketSinkStore::Instance().PushBatch(pipeline_key_, batch,
                                                 max_buffer_size_);
        return Status::Ok();
    }

    // 静态方法：获取指定流水线的最新批次（供外部 WebSocket 服务调用）
    // 参数:
    //   pipeline_key - 流水线标识符
    // 返回:
    //   最新 DataBatch 指针
    static DataBatchPtr PollLatest(const std::string& pipeline_key) {
        return WebSocketSinkStore::Instance().Latest(pipeline_key);
    }

    // 静态方法：获取指定流水线的最近 N 条批次（供外部 WebSocket 服务调用）
    // 参数:
    //   pipeline_key - 流水线标识符
    //   max_n        - 最多返回条数
    // 返回:
    //   最近 N 条 DataBatch 的 vector
    static std::vector<DataBatchPtr> PollRecent(const std::string& pipeline_key,
                                                size_t max_n) {
        return WebSocketSinkStore::Instance().SnapshotRecent(pipeline_key,
                                                             max_n);
    }

private:
    size_t max_buffer_size_ = 100;     // 缓冲区最大容量
    std::string pipeline_key_ = "default"; // 流水线标识符
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("websocket_sink", WebSocketSink);

}  // namespace illuminator
