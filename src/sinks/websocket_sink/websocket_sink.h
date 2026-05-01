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

class WebSocketSinkStore {
public:
    static WebSocketSinkStore& Instance() {
        static WebSocketSinkStore inst;
        return inst;
    }

    void PushBatch(const std::string& pipeline_key, DataBatchPtr batch,
                   size_t max_keep) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& dq = buffers_[pipeline_key];
        dq.push_back(batch);
        while (dq.size() > max_keep)
            dq.pop_front();
    }

    DataBatchPtr Latest(const std::string& pipeline_key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = buffers_.find(pipeline_key);
        if (it == buffers_.end() || it->second.empty())
            return nullptr;
        return it->second.back();
    }

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
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::deque<DataBatchPtr>> buffers_;
};

class WebSocketSink : public SinkPlugin {
public:
    const char* Name() const override { return "websocket_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        max_buffer_size_ =
            static_cast<size_t>(config["max_buffer_size"].AsInt(100));
        pipeline_key_ = config["pipeline_key"].AsString("default");
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch)
            return Status::Ok();
        WebSocketSinkStore::Instance().PushBatch(pipeline_key_, batch,
                                                 max_buffer_size_);
        return Status::Ok();
    }

    static DataBatchPtr PollLatest(const std::string& pipeline_key) {
        return WebSocketSinkStore::Instance().Latest(pipeline_key);
    }

    static std::vector<DataBatchPtr> PollRecent(const std::string& pipeline_key,
                                                size_t max_n) {
        return WebSocketSinkStore::Instance().SnapshotRecent(pipeline_key,
                                                             max_n);
    }

private:
    size_t max_buffer_size_ = 100;
    std::string pipeline_key_ = "default";
};

IL_REGISTER_SINK("websocket_sink", WebSocketSink);

}  // namespace illuminator
