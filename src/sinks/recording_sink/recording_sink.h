// =============================================================================
// RecordingSink — 按需录制落盘 Sink
// =============================================================================
// 通过 API 控制的数据录制功能：
//   - 用户在前端点击"录制"开始，点击"停止"结束
//   - 数据以 NDJSON（newline-delimited JSON）格式追加写入文件
//   - 支持硬性大小限制，超限自动停止
//   - 每个 Feature 独立录制文件
//
// 文件格式（.ilr — Illuminator Recording）:
//   第一行: {"format":"ilr","version":1,"feature":"xxx","started_at":...}
//   后续行: {"ts":..., "data": {...}} （每个 DataBatch 一行）
//
// 设计考虑:
//   - 与 SseSink 并联，录制不影响实时流性能
//   - 录制不影响实时流性能（独立文件 I/O）
//   - 支持多次录制（同一 Feature 多个录制文件）
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/common/logging.h"
#include "plugin/api/sink_plugin.h"
#include "serialization/json_serializer.h"

namespace illuminator {

struct RecordingSession {
    std::string file_path;
    std::string feature_name;
    std::chrono::system_clock::time_point started_at;
    uint64_t bytes_written = 0;
    uint64_t batches_written = 0;
    uint64_t max_bytes = 0;
    bool active = false;
};

class RecordingSink : public SinkPlugin {
public:
    const char* Name() const override { return "recording_sink"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        feature_name_ = config["feature_name"].AsString("default");
        output_dir_ = config["output_dir"].AsString("/tmp/illuminator_recordings");
        max_file_bytes_ = static_cast<uint64_t>(
            config["max_file_mb"].AsInt(100)) * 1024ULL * 1024ULL;
        return Status::Ok();
    }

    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        std::lock_guard<std::mutex> lk(mu_);
        if (!recording_) return Status::Ok();

        nlohmann::json j;
        j["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        j["data"] = nlohmann::json::parse(BatchToJson(*batch, feature_name_));

        std::string line = j.dump() + "\n";
        if (session_.bytes_written + line.size() > max_file_bytes_) {
            IL_WARN("RecordingSink: max file size reached ({}MB), stopping recording",
                    max_file_bytes_ / (1024 * 1024));
            StopRecordingLocked();
            return Status::Ok();
        }

        if (file_) {
            file_->write(line.data(), line.size());
            file_->flush();
            session_.bytes_written += line.size();
            session_.batches_written++;
        }
        return Status::Ok();
    }

    // 开始录制
    Status StartRecording() {
        std::lock_guard<std::mutex> lk(mu_);
        if (recording_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Already recording");
        }

        auto now = std::chrono::system_clock::now();
        auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        std::string filename = feature_name_ + "_" +
                               std::to_string(epoch_ms) + ".ilr";
        std::string path = output_dir_ + "/" + filename;

        // 确保输出目录存在
        std::string mkdir_cmd = "mkdir -p " + output_dir_;
        (void)std::system(mkdir_cmd.c_str());

        file_ = std::make_unique<std::ofstream>(path, std::ios::binary);
        if (!file_->is_open()) {
            file_.reset();
            return Status::Error(StatusCode::kInternal,
                                 "Cannot open recording file: " + path);
        }

        session_ = {};
        session_.file_path = path;
        session_.feature_name = feature_name_;
        session_.started_at = now;
        session_.max_bytes = max_file_bytes_;
        session_.active = true;

        // 写入文件头
        nlohmann::json header;
        header["format"] = "ilr";
        header["version"] = 1;
        header["feature"] = feature_name_;
        header["started_at"] = epoch_ms;
        header["max_bytes"] = max_file_bytes_;

        std::string header_line = header.dump() + "\n";
        file_->write(header_line.data(), header_line.size());
        session_.bytes_written += header_line.size();

        recording_ = true;
        IL_INFO("RecordingSink: started recording to {}", path);
        return Status::Ok();
    }

    // 停止录制
    Status StopRecording() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!recording_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Not recording");
        }
        StopRecordingLocked();
        return Status::Ok();
    }

    bool IsRecording() const {
        std::lock_guard<std::mutex> lk(mu_);
        return recording_;
    }

    RecordingSession GetSession() const {
        std::lock_guard<std::mutex> lk(mu_);
        return session_;
    }

    void SetFeatureName(const std::string& name) { feature_name_ = name; }
    void SetOutputDir(const std::string& dir) { output_dir_ = dir; }

private:
    void StopRecordingLocked() {
        if (file_) {
            file_->flush();
            file_.reset();
        }
        recording_ = false;
        session_.active = false;
        IL_INFO("RecordingSink: stopped recording {} ({} batches, {} bytes)",
                session_.file_path, session_.batches_written,
                session_.bytes_written);
    }

    mutable std::mutex mu_;
    std::string feature_name_ = "default";
    std::string output_dir_ = "/tmp/illuminator_recordings";
    uint64_t max_file_bytes_ = 100ULL * 1024 * 1024;

    bool recording_ = false;
    RecordingSession session_;
    std::unique_ptr<std::ofstream> file_;
};

// RecordingSinkRegistry — 按 Feature 名管理 RecordingSink 实例引用（支持 API 访问）
// 注意：不拥有 RecordingSink 的所有权，Pipeline 拥有所有权。
// Feature Stop 时必须 Unregister 以避免悬挂指针。
class RecordingSinkRegistry {
public:
    static RecordingSinkRegistry& Instance() {
        static RecordingSinkRegistry inst;
        return inst;
    }

    void Register(const std::string& feature_name, RecordingSink* sink) {
        std::lock_guard<std::mutex> lk(mu_);
        sinks_[feature_name] = sink;
    }

    void Unregister(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        sinks_.erase(feature_name);
    }

    std::shared_ptr<RecordingSink> Get(const std::string& feature_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sinks_.find(feature_name);
        if (it == sinks_.end() || !it->second) return nullptr;
        // 返回不拥有所有权的 shared_ptr（custom deleter = no-op）
        return std::shared_ptr<RecordingSink>(
            it->second, [](RecordingSink*) {});
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
    std::unordered_map<std::string, RecordingSink*> sinks_;
};

}  // namespace illuminator
