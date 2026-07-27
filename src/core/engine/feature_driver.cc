#include "core/engine/feature_driver.h"

namespace illuminator {
namespace {

SsePublishCallback& SsePublishCallbackStorage() {
    static SsePublishCallback cb;
    return cb;
}

SseSinkFactory& SseSinkFactoryStorage() {
    static SseSinkFactory factory;
    return factory;
}

RecordingSinkFactory& RecordingSinkFactoryStorage() {
    static RecordingSinkFactory factory;
    return factory;
}

std::string& DefaultDataDirStorage() {
    static std::string dir = "/var/lib/illuminator";
    return dir;
}

}  // namespace

void FeatureDriver::SetSsePublishCallback(SsePublishCallback cb) {
    SsePublishCallbackStorage() = std::move(cb);
}

SsePublishCallback& FeatureDriver::GetSsePublishCallback() {
    return SsePublishCallbackStorage();
}

void FeatureDriver::SetSseSinkFactory(SseSinkFactory factory) {
    SseSinkFactoryStorage() = std::move(factory);
}

SseSinkFactory& FeatureDriver::GetSseSinkFactory() {
    return SseSinkFactoryStorage();
}

void FeatureDriver::SetRecordingSinkFactory(RecordingSinkFactory factory) {
    RecordingSinkFactoryStorage() = std::move(factory);
}

RecordingSinkFactory& FeatureDriver::GetRecordingSinkFactory() {
    return RecordingSinkFactoryStorage();
}

void FeatureDriver::SetDefaultDataDir(const std::string& dir) {
    DefaultDataDirStorage() = dir;
}

const std::string& FeatureDriver::GetDefaultDataDir() {
    return DefaultDataDirStorage();
}

std::unique_ptr<SinkPlugin> FeatureDriver::MakeSseSink(const char* feature_name) {
    auto& factory = SseSinkFactoryStorage();
    if (!factory) {
        return nullptr;
    }
    return factory(feature_name);
}

Status FeatureDriver::StartRecording(const std::string& output_dir) {
    if (state_.load(std::memory_order_acquire) == DriverState::kInactive ||
        !pipeline_) {
        return Status::Error(StatusCode::kInvalidArgument, "Feature not active");
    }
    if (recording_sink_) {
        return Status::Error(StatusCode::kAlreadyExists, "Already recording");
    }

    auto& factory = RecordingSinkFactoryStorage();
    if (!factory) {
        return Status::Error(StatusCode::kUnavailable, "Recording not configured");
    }

    const std::string& dir = output_dir.empty()
        ? DefaultDataDirStorage() : output_dir;
    auto [sink, recordable] = factory(std::string(Name()), dir);
    if (!sink || !recordable) {
        return Status::Error(StatusCode::kInternal, "Recording factory returned null");
    }

    recording_sink_ = recordable;
    RecordingSinkRegistry::Instance().Register(Name(), recording_sink_);
    auto status = pipeline_->AddSinkRuntime(std::move(sink));
    if (!status.ok()) {
        RecordingSinkRegistry::Instance().Unregister(Name());
        recording_sink_ = nullptr;
        return status;
    }

    recording_sink_->StartRecording();
    return Status::Ok();
}

Status FeatureDriver::StopRecording() {
    if (!recording_sink_) {
        return Status::Error(StatusCode::kNotFound, "Not recording");
    }

    recording_sink_->StopRecording();
    RecordingSinkRegistry::Instance().Unregister(Name());
    pipeline_->RemoveSinkRuntime("recording");
    recording_sink_ = nullptr;
    return Status::Ok();
}

bool FeatureDriver::IsRecording() const {
    return recording_sink_ != nullptr && recording_sink_->IsRecording();
}

}  // namespace illuminator
