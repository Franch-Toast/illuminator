#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace illuminator {

struct RecordingSession {
    std::string file_path;
    std::string feature_name;
    uint64_t bytes_written = 0;
    uint64_t batches_written = 0;
};

class RecordableInterface {
public:
    virtual ~RecordableInterface() = default;
    virtual void StartRecording() = 0;
    virtual void StopRecording() = 0;
    virtual bool IsRecording() const = 0;
    virtual RecordingSession GetSession() const = 0;
};

// RecordingSinkRegistry — 按 Feature 名管理 RecordableInterface 实例引用
// Pipeline 拥有 Sink 所有权，此处仅持有弱引用供 API 查询。
class RecordingSinkRegistry {
public:
    static RecordingSinkRegistry& Instance() {
        static RecordingSinkRegistry inst;
        return inst;
    }

    void Register(const std::string& feature_name, RecordableInterface* sink) {
        std::lock_guard<std::mutex> lk(mu_);
        sinks_[feature_name] = sink;
    }

    void Unregister(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        sinks_.erase(feature_name);
    }

    RecordableInterface* Get(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sinks_.find(feature_name);
        return it != sinks_.end() ? it->second : nullptr;
    }

    std::vector<std::string> ListNames() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> names;
        names.reserve(sinks_.size());
        for (const auto& [name, sink] : sinks_) {
            if (sink) names.push_back(name);
        }
        return names;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, RecordableInterface*> sinks_;
};

}  // namespace illuminator
