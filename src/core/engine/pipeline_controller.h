#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/data_batch.h"
#include "core/memory/lock_free_queue.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

// A single pipeline instance: Source → Processors → Aggregator → Sinks
class Pipeline {
public:
    explicit Pipeline(const std::string& name) : name_(name) {}
    ~Pipeline() { Stop(); }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    const std::string& name() const { return name_; }

    void SetSource(std::unique_ptr<SourcePlugin> source) {
        source_ = std::move(source);
    }

    void AddProcessor(std::unique_ptr<ProcessorPlugin> proc) {
        processors_.push_back(std::move(proc));
    }

    void SetAggregator(std::unique_ptr<AggregatorPlugin> agg) {
        aggregator_ = std::move(agg);
    }

    void AddSink(std::unique_ptr<SinkPlugin> sink) {
        sinks_.push_back(std::move(sink));
    }

    Status Start() {
        if (!source_) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no source: " + name_);
        }
        if (sinks_.empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Pipeline has no sinks: " + name_);
        }

        running_.store(true, std::memory_order_release);

        auto status = source_->Start();
        if (!status.ok()) return status;

        for (auto& p : processors_) {
            status = p->Start();
            if (!status.ok()) return status;
        }
        if (aggregator_) {
            status = aggregator_->Start();
            if (!status.ok()) return status;
        }
        for (auto& s : sinks_) {
            status = s->Start();
            if (!status.ok()) return status;
        }

        if (source_->IsPushMode()) {
            source_->SetCallback([this](DataBatchPtr batch) {
                OnBatchReceived(std::move(batch));
            });
        } else {
            collect_thread_ = std::thread([this] { CollectLoop(); });
        }

        if (aggregator_) {
            flush_thread_ = std::thread([this] { FlushLoop(); });
        }

        IL_INFO("Pipeline '%s' started", name_.c_str());
        return Status::Ok();
    }

    Status Stop() {
        if (!running_.exchange(false)) return Status::Ok();

        if (collect_thread_.joinable()) collect_thread_.join();
        if (flush_thread_.joinable()) flush_thread_.join();

        source_->Stop();
        for (auto& p : processors_) p->Stop();
        if (aggregator_) aggregator_->Stop();
        for (auto& s : sinks_) {
            s->Flush();
            s->Stop();
        }

        IL_INFO("Pipeline '%s' stopped", name_.c_str());
        return Status::Ok();
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // Stats
    uint64_t BatchesProcessed() const { return batches_processed_.load(); }
    uint64_t RecordsProcessed() const { return records_processed_.load(); }
    uint64_t ErrorCount() const { return error_count_.load(); }

private:
    void CollectLoop() {
        while (running_.load(std::memory_order_acquire)) {
            auto result = source_->Collect();
            if (result.ok()) {
                OnBatchReceived(std::move(result.value()));
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(source_->IntervalMs()));
        }
    }

    void FlushLoop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(aggregator_->FlushIntervalMs()));
            auto result = aggregator_->Flush();
            if (result.ok()) {
                for (auto& batch : result.value()) {
                    DeliverToSinks(std::move(batch));
                }
            }
        }
        // Final flush
        auto result = aggregator_->Flush();
        if (result.ok()) {
            for (auto& batch : result.value()) {
                DeliverToSinks(std::move(batch));
            }
        }
    }

    void OnBatchReceived(DataBatchPtr batch) {
        if (!batch || batch->Empty()) return;

        records_processed_.fetch_add(batch->Size(), std::memory_order_relaxed);

        // Run through processor chain
        for (auto& proc : processors_) {
            auto result = proc->Process(std::move(batch));
            if (!result.ok()) {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            batch = std::move(result.value());
            if (!batch || batch->Empty()) return;
        }

        if (aggregator_) {
            aggregator_->Add(std::move(batch));
        } else {
            DeliverToSinks(std::move(batch));
        }

        batches_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    void DeliverToSinks(DataBatchPtr batch) {
        for (size_t i = 0; i < sinks_.size(); ++i) {
            // Last sink gets ownership; others get a shared copy
            auto status = sinks_[i]->Write(batch);
            if (!status.ok()) {
                IL_WARN("Sink write error in pipeline '%s': %s",
                        name_.c_str(), status.message().c_str());
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::string name_;
    std::atomic<bool> running_{false};

    std::unique_ptr<SourcePlugin> source_;
    std::vector<std::unique_ptr<ProcessorPlugin>> processors_;
    std::unique_ptr<AggregatorPlugin> aggregator_;
    std::vector<std::unique_ptr<SinkPlugin>> sinks_;

    std::thread collect_thread_;
    std::thread flush_thread_;

    std::atomic<uint64_t> batches_processed_{0};
    std::atomic<uint64_t> records_processed_{0};
    std::atomic<uint64_t> error_count_{0};
};

// Manages multiple pipelines based on configuration
class PipelineController {
public:
    Status BuildFromConfig(const GlobalConfig& config);
    Status StartAll();
    Status StopAll();

    Pipeline* GetPipeline(const std::string& name);
    const std::vector<std::unique_ptr<Pipeline>>& Pipelines() const {
        return pipelines_;
    }

private:
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
};

}  // namespace illuminator
