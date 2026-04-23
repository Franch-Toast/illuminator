#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

Status PipelineController::BuildFromConfig(const GlobalConfig& config) {
    auto& registry = PluginRegistry::Instance();

    for (auto& pc : config.pipelines) {
        auto pipeline = std::make_unique<Pipeline>(pc.name);

        // Create source
        auto source = registry.CreateSource(pc.source.type);
        if (!source) {
            return Status::Error(StatusCode::kNotFound,
                "Source plugin not found: " + pc.source.type +
                " (pipeline: " + pc.name + ")");
        }
        auto status = source->Init(pc.source.config);
        if (!status.ok()) return status;
        pipeline->SetSource(std::move(source));

        // Create processors
        for (auto& proc_cfg : pc.processors) {
            auto proc = registry.CreateProcessor(proc_cfg.type);
            if (!proc) {
                return Status::Error(StatusCode::kNotFound,
                    "Processor plugin not found: " + proc_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = proc->Init(proc_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddProcessor(std::move(proc));
        }

        // Create aggregator (optional)
        if (pc.aggregator.has_value()) {
            auto agg = registry.CreateAggregator(pc.aggregator->type);
            if (!agg) {
                return Status::Error(StatusCode::kNotFound,
                    "Aggregator plugin not found: " + pc.aggregator->type +
                    " (pipeline: " + pc.name + ")");
            }
            status = agg->Init(pc.aggregator->config);
            if (!status.ok()) return status;
            pipeline->SetAggregator(std::move(agg));
        }

        // Create sinks
        for (auto& sink_cfg : pc.sinks) {
            auto sink = registry.CreateSink(sink_cfg.type);
            if (!sink) {
                return Status::Error(StatusCode::kNotFound,
                    "Sink plugin not found: " + sink_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = sink->Init(sink_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddSink(std::move(sink));
        }

        IL_INFO("Built pipeline: %s", pc.name.c_str());
        pipelines_.push_back(std::move(pipeline));
    }

    return Status::Ok();
}

Status PipelineController::StartAll() {
    for (auto& pipeline : pipelines_) {
        auto status = pipeline->Start();
        if (!status.ok()) {
            IL_ERROR("Failed to start pipeline '%s': %s",
                     pipeline->name().c_str(), status.message().c_str());
            StopAll();
            return status;
        }
    }
    IL_INFO("All %zu pipelines started", pipelines_.size());
    return Status::Ok();
}

Status PipelineController::StopAll() {
    for (auto& pipeline : pipelines_) {
        pipeline->Stop();
    }
    IL_INFO("All pipelines stopped");
    return Status::Ok();
}

Pipeline* PipelineController::GetPipeline(const std::string& name) {
    for (auto& p : pipelines_) {
        if (p->name() == name) return p.get();
    }
    return nullptr;
}

}  // namespace illuminator
