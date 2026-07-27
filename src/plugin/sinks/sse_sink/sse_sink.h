// ============================================================================
// SseSink — Pipeline Sink 将数据推送到 SSE 订阅者
// ============================================================================
// 在 SinkPool 线程中执行，通过注入的 publish callback 推送数据，不阻塞。
// callback 通常由 main 注入并绑定到 SseHandler::Publish()。

#pragma once

#include <string>
#include <utility>

#include "core/common/json_serializer.h"
#include "core/common/data_batch.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

class SseSink : public SinkPlugin {
public:
    SseSink(std::string feature_name, SsePublishCallback publish_cb)
        : feature_name_(std::move(feature_name)),
          publish_cb_(std::move(publish_cb)) {}

    const char* Name() const override { return "sse_sink"; }
    const char* Version() const override { return "1.0.0"; }

    Status Write(ConstDataBatchPtr batch) override {
        if (!batch) return Status::Ok();
        if (publish_cb_) {
            publish_cb_(feature_name_, SerializeBatch(*batch));
        }
        return Status::Ok();
    }

private:
    std::string SerializeBatch(const DataBatch& batch) const {
        return BatchToSseJson(batch, feature_name_, seq_++);
    }

    std::string feature_name_;
    SsePublishCallback publish_cb_;
    mutable uint64_t seq_ = 0;
};

}  // namespace illuminator
