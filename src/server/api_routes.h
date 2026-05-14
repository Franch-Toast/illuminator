// ============================================================================
// Illuminator HTTP API 路由注册
// ============================================================================
// 将所有 REST API 端点注册到 httplib::Server。
// 拆分自 main.cc，职责单一化。
// ============================================================================

#pragma once

#include <string>

#include "httplib.h"

#include "core/common/self_observability.h"
#include "core/engine/pipeline_controller.h"
#include "serialization/json_serializer.h"

namespace illuminator {

static constexpr const char* kIlluminatorVersion = "0.1.0";

inline void JsonError(httplib::Response& res, const std::string& msg,
                      int status = 500) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump() + "\n", "application/json");
}

inline void RegisterApiRoutes(httplib::Server& srv,
                              PipelineController& controller) {
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status", "ok"}, {"version", kIlluminatorVersion}}.dump() + "\n",
            "application/json");
    });

    srv.Get("/api/v1/pipelines",
            [&controller](const httplib::Request&, httplib::Response& res) {
                json arr = json::array();
                for (auto& p : controller.Pipelines()) {
                    auto* src = p->GetSource();
                    arr.push_back({
                        {"name", p->name()},
                        {"running", p->IsRunning()},
                        {"stub", src ? src->IsStub() : false},
                        {"batches", p->BatchesProcessed()},
                        {"records", p->RecordsProcessed()},
                        {"errors", p->ErrorCount()},
                        {"channel", {
                            {"capacity", p->ChannelCapacity()},
                            {"size", p->ChannelSize()},
                            {"enqueued", p->ChannelEnqueued()},
                            {"dequeued", p->ChannelDequeued()},
                            {"dropped", p->ChannelDropped()},
                            {"backpressure_events", p->ChannelBackpressureEvents()},
                            {"backpressured", p->ChannelBackpressured()},
                        }},
                    });
                }
                res.set_content(
                    json{{"pipelines", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

    // Generic collect endpoint: /api/v1/pipelines/:name/collect
    auto collect_handler = [&controller](const httplib::Request& req,
                                         httplib::Response& res) {
        auto name = req.path_params.at("name");
        auto* pipe = controller.GetPipeline(name);
        if (!pipe) {
            JsonError(res, "pipeline '" + name + "' not found", 404);
            return;
        }
        auto* source = pipe->GetSource();
        if (!source) {
            JsonError(res, "pipeline '" + name + "' has no source");
            return;
        }
        auto result = source->Collect();
        if (!result.ok()) {
            JsonError(res, result.status().message());
            return;
        }
        auto processed = pipe->RunProcessors(std::move(*result));
        if (!processed.ok()) {
            JsonError(res, processed.status().message());
            return;
        }
        res.set_content(BatchToJson(**processed, name) + "\n",
                        "application/json");
    };
    srv.Get("/api/v1/pipelines/:name/collect", collect_handler);

    // Legacy convenience aliases (delegate to the same pipeline names)
    auto pipeline_collect = [&controller](const std::string& pipeline_name,
                                           httplib::Response& res) {
        auto* pipe = controller.GetPipeline(pipeline_name);
        if (!pipe) {
            JsonError(res, "pipeline '" + pipeline_name + "' not found", 404);
            return;
        }
        auto* source = pipe->GetSource();
        if (!source) {
            JsonError(res, "pipeline '" + pipeline_name + "' has no source");
            return;
        }
        auto result = source->Collect();
        if (!result.ok()) {
            JsonError(res, result.status().message());
            return;
        }
        auto processed = pipe->RunProcessors(std::move(*result));
        if (!processed.ok()) {
            JsonError(res, processed.status().message());
            return;
        }
        res.set_content(BatchToJson(**processed, pipeline_name) + "\n",
                        "application/json");
    };

    srv.Get("/api/v1/cpu/utilization",
            [pipeline_collect](const httplib::Request&, httplib::Response& res) {
                pipeline_collect("cpu_utilization", res);
            });
    srv.Get("/api/v1/cpu/processes",
            [pipeline_collect](const httplib::Request&, httplib::Response& res) {
                pipeline_collect("cpu_processes", res);
            });
    srv.Get("/api/v1/cpu/profile/flamegraph",
            [pipeline_collect](const httplib::Request&, httplib::Response& res) {
                pipeline_collect("cpu_profile", res);
            });
    srv.Get("/api/v1/cpu/profile/offcpu",
            [pipeline_collect](const httplib::Request&, httplib::Response& res) {
                pipeline_collect("offcpu_analysis", res);
            });
    srv.Get("/api/v1/cpu/sched/summary",
            [pipeline_collect](const httplib::Request&, httplib::Response& res) {
                pipeline_collect("sched_analysis", res);
            });

    // QueryExtra endpoints
    auto query_handler = [&controller](const std::string& pipeline_name,
                                        const std::string& query_name,
                                        const httplib::Request& req,
                                        httplib::Response& res) {
        auto* pipe = controller.GetPipeline(pipeline_name);
        if (!pipe) { JsonError(res, pipeline_name + " pipeline not found", 404); return; }
        auto* source = pipe->GetSource();
        if (!source) { JsonError(res, "source not available"); return; }
        QueryParams params;
        for (auto& [k, v] : req.params) params[k] = v;
        auto result = source->QueryExtra(query_name, params);
        if (!result.ok()) { JsonError(res, result.status().message()); return; }
        res.set_content(*result + "\n", "application/json");
    };

    srv.Get("/api/v1/cpu/sched/history",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "history", req, res);
            });
    srv.Get("/api/v1/cpu/sched/events",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "events", req, res);
            });
    srv.Get("/api/v1/cpu/sched/wakeups",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "wakeups", req, res);
            });

    // Channel stats endpoint
    srv.Get("/api/v1/channel_stats",
            [&controller](const httplib::Request&, httplib::Response& res) {
                json arr = json::array();
                for (auto& p : controller.Pipelines()) {
                    arr.push_back({
                        {"pipeline", p->name()},
                        {"capacity", p->ChannelCapacity()},
                        {"size", p->ChannelSize()},
                        {"utilization", p->ChannelCapacity() > 0
                            ? static_cast<double>(p->ChannelSize()) / p->ChannelCapacity()
                            : 0.0},
                        {"enqueued", p->ChannelEnqueued()},
                        {"dequeued", p->ChannelDequeued()},
                        {"dropped", p->ChannelDropped()},
                        {"backpressure_events", p->ChannelBackpressureEvents()},
                        {"backpressured", p->ChannelBackpressured()},
                    });
                }
                res.set_content(
                    json{{"channels", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

    // Metrics endpoints
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(InternalMetrics::Instance().ExportPrometheus(),
                        "text/plain");
    });
    srv.Get("/api/v1/internal_metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(InternalMetrics::Instance().ExportJson() + "\n",
                        "application/json");
    });
}

}  // namespace illuminator
