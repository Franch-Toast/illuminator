#pragma once

#include <memory>
#include <utility>

#include "core/engine/feature_driver.h"
#include "plugin/api/recording_interface.h"
#include "plugin/sinks/recording_sink/recording_sink.h"
#include "plugin/sinks/sse_sink/sse_sink.h"

namespace illuminator {

// SSE Sink 工厂：使用全局 SSE publish callback
inline std::unique_ptr<SinkPlugin> MakeFeatureSseSink(const char* feature_name) {
    return std::make_unique<SseSink>(feature_name, FeatureDriver::GetSsePublishCallback());
}

// Recording Sink 工厂：创建并初始化 RecordingSink
inline std::unique_ptr<RecordingSink> MakeFeatureRecordingSink(
    const std::string& feature_name, const std::string& output_dir) {
    auto rec_sink = std::make_unique<RecordingSink>();
    ConfigValue cfg;
    cfg.Set("feature_name", feature_name);
    cfg.Set("output_dir", output_dir);
    rec_sink->Init(cfg);
    return rec_sink;
}

inline std::pair<std::unique_ptr<SinkPlugin>, RecordableInterface*>
MakeFeatureRecordingSinkPair(const std::string& feature_name,
                             const std::string& output_dir) {
    auto rec_sink = MakeFeatureRecordingSink(feature_name, output_dir);
    auto* iface = rec_sink.get();
    return {std::move(rec_sink), iface};
}

}  // namespace illuminator
