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
#include "core/common/version_generated.h"
#include "core/engine/feature_manager.h"
#include "core/engine/pipeline_controller.h"
#include "serialization/json_serializer.h"
#include "sinks/recording_sink/recording_sink.h"
#include "sinks/stream_sink/stream_sink.h"
#include "storage/storage_backend.h"

namespace illuminator {

static constexpr const char* kIlluminatorVersion = kBuildVersion;

inline void JsonError(httplib::Response& res, const std::string& msg,
                      int status = 500) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump() + "\n", "application/json");
}

inline void SetupAuthMiddleware(httplib::Server& srv,
                                const std::string& auth_token) {
    if (auth_token.empty()) return;

    srv.set_pre_routing_handler(
        [auth_token](const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/healthz" || req.path == "/metrics") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (req.path.find("/api/") != 0) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            auto it = req.headers.find("Authorization");
            if (it == req.headers.end()) {
                res.status = 401;
                res.set_content(R"({"error":"missing Authorization header"})",
                                "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            std::string expected = "Bearer " + auth_token;
            if (it->second != expected) {
                res.status = 403;
                res.set_content(R"({"error":"invalid token"})",
                                "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
}

inline void RegisterApiRoutes(httplib::Server& srv,
                              PipelineController& controller) {
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status", "ok"}, {"version", kIlluminatorVersion},
                 {"commit", kBuildCommit}}.dump() + "\n",
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
                            {"flush_injected", p->ChannelFlushInjected()},
                            {"backpressure_events", p->ChannelBackpressureEvents()},
                            {"backpressured", p->ChannelBackpressured()},
                        }},
                    });
                }
                res.set_content(
                    json{{"pipelines", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

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

    srv.Get("/api/v1/pipelines/:name/collect",
            [pipeline_collect](const httplib::Request& req, httplib::Response& res) {
                pipeline_collect(req.path_params.at("name"), res);
            });

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
                pipeline_collect("offcpu_profile", res);
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

    srv.Get("/api/v1/cpu/profile/offcpu/snapshot",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("offcpu_profile", "snapshot", req, res);
            });
    srv.Get("/api/v1/cpu/profile/oncpu/snapshot",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("cpu_profile", "snapshot", req, res);
            });
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
                        {"flush_injected", p->ChannelFlushInjected()},
                        {"backpressure_events", p->ChannelBackpressureEvents()},
                        {"backpressured", p->ChannelBackpressured()},
                    });
                }
                res.set_content(
                    json{{"channels", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

    // SQL Query endpoint — executes read-only SQL against the storage backend
    srv.Post("/api/v1/query",
            [&controller](const httplib::Request& req, httplib::Response& res) {
                try {
                    auto body = json::parse(req.body);
                    std::string sql = body.value("query", "");
                    if (sql.empty()) {
                        JsonError(res, "missing 'query' field", 400);
                        return;
                    }
                    // Basic safety: only allow SELECT queries
                    std::string upper;
                    for (size_t i = 0; i < sql.size() && i < 20; ++i)
                        upper += static_cast<char>(std::toupper(sql[i]));
                    if (upper.find("SELECT") == std::string::npos &&
                        upper.find("PRAGMA") == std::string::npos) {
                        JsonError(res, "only SELECT queries are allowed", 403);
                        return;
                    }

                    auto* storage = controller.GetStorageBackend();
                    if (!storage) {
                        JsonError(res, "no storage backend available");
                        return;
                    }
                    auto result = storage->ExecuteRawQuery(sql);
                    if (!result.ok()) {
                        JsonError(res, result.status().message());
                        return;
                    }
                    res.set_content(*result + "\n", "application/json");
                } catch (const std::exception& e) {
                    JsonError(res, std::string("query parse error: ") + e.what(), 400);
                }
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

// ============================================================================
// Feature API — 按需启停 + 实时流控制
// ============================================================================
inline void RegisterFeatureRoutes(httplib::Server& srv,
                                   FeatureManager& features) {
    srv.Get("/api/v1/features",
            [&features](const httplib::Request&, httplib::Response& res) {
                auto list = features.ListFeatures();
                json arr = json::array();
                for (const auto& f : list) {
                    arr.push_back({
                        {"name", f.name},
                        {"display_name", f.display_name},
                        {"category", f.category},
                        {"state", FeatureStateToString(f.state)},
                        {"is_recording", f.is_recording},
                        {"batches_processed", f.batches_processed},
                        {"records_processed", f.records_processed},
                        {"errors", f.errors},
                        {"uptime_ms", f.uptime_ms},
                    });
                }
                res.set_content(
                    json{{"features", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

    srv.Post("/api/v1/features/:name/start",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto status = features.Start(name);
                 if (!status.ok()) {
                     int code = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                     JsonError(res, status.message(), code);
                     return;
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"state", "active"}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/features/:name/stop",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto status = features.Stop(name);
                 if (!status.ok()) {
                     int code = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                     JsonError(res, status.message(), code);
                     return;
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"state", "inactive"}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/features/:name/pause",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto status = features.Pause(name);
                 if (!status.ok()) {
                     int code = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                     JsonError(res, status.message(), code);
                     return;
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"state", "paused"}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/features/:name/resume",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto status = features.Resume(name);
                 if (!status.ok()) {
                     int code = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                     JsonError(res, status.message(), code);
                     return;
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"state", "active"}}.dump() + "\n",
                     "application/json");
             });

    srv.Get("/api/v1/features/:name/collect",
            [&features](const httplib::Request& req, httplib::Response& res) {
                auto name = req.path_params.at("name");
                auto state = features.GetState(name);
                if (state == FeatureState::kInactive) {
                    JsonError(res, "Feature '" + name + "' is not active", 400);
                    return;
                }

                // 从 StreamSinkStore 拉取最新数据（不干扰 pipeline 正常采集）
                auto& buf = StreamSinkStore::Instance().GetBuffer(name);
                auto recent = buf.Recent(1);
                if (recent.empty()) {
                    res.set_content(
                        json{{"feature", name}, {"data", json::array()},
                             {"seq", buf.Sequence()}}.dump() + "\n",
                        "application/json");
                    return;
                }
                res.set_content(
                    BatchToJson(*recent.back(), name) + "\n",
                    "application/json");
            });

    // 增量数据拉取（支持 cursor 参数避免重复消费）
    srv.Get("/api/v1/features/:name/stream",
            [](const httplib::Request& req, httplib::Response& res) {
                auto name = req.path_params.at("name");
                uint64_t cursor = 0;
                if (req.has_param("cursor")) {
                    cursor = std::stoull(req.get_param_value("cursor"));
                }

                auto& buf = StreamSinkStore::Instance().GetBuffer(name);
                auto batches = buf.PollSince(cursor);

                json j;
                j["feature"] = name;
                j["cursor"] = cursor;
                json arr = json::array();
                for (auto& b : batches) {
                    arr.push_back(json::parse(BatchToJson(*b, name)));
                }
                j["batches"] = std::move(arr);
                res.set_content(j.dump() + "\n", "application/json");
            });

    // === 录制 API ===
    srv.Post("/api/v1/features/:name/record/start",
             [](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto sink = RecordingSinkRegistry::Instance().Get(name);
                 if (!sink) {
                     JsonError(res, "No recording sink for feature: " + name, 404);
                     return;
                 }
                 auto status = sink->StartRecording();
                 if (!status.ok()) {
                     JsonError(res, status.message(), 400);
                     return;
                 }
                 auto session = sink->GetSession();
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"file", session.file_path}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/features/:name/record/stop",
             [](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto sink = RecordingSinkRegistry::Instance().Get(name);
                 if (!sink) {
                     JsonError(res, "No recording sink for feature: " + name, 404);
                     return;
                 }
                 auto status = sink->StopRecording();
                 if (!status.ok()) {
                     JsonError(res, status.message(), 400);
                     return;
                 }
                 auto session = sink->GetSession();
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"file", session.file_path},
                          {"batches", session.batches_written},
                          {"bytes", session.bytes_written}}.dump() + "\n",
                     "application/json");
             });

    srv.Get("/api/v1/features/:name/record/status",
            [](const httplib::Request& req, httplib::Response& res) {
                auto name = req.path_params.at("name");
                auto sink = RecordingSinkRegistry::Instance().Get(name);
                if (!sink) {
                    res.set_content(
                        json{{"feature", name}, {"recording", false}}.dump() + "\n",
                        "application/json");
                    return;
                }
                bool recording = sink->IsRecording();
                auto session = sink->GetSession();
                json j;
                j["feature"] = name;
                j["recording"] = recording;
                if (recording) {
                    j["file"] = session.file_path;
                    j["bytes_written"] = session.bytes_written;
                    j["batches_written"] = session.batches_written;
                }
                res.set_content(j.dump() + "\n", "application/json");
            });
}

}  // namespace illuminator
