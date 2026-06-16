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
#include "plugin/manager/plugin_manager.h"
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
                res.set_header("Deprecation", "true");
                res.set_header("Sunset", "2026-09-01");
                res.set_header("Link",
                    "</api/v1/features/" + req.path_params.at("name") +
                    "/collect>; rel=\"successor-version\"");
                pipeline_collect(req.path_params.at("name"), res);
            });

    auto deprecated_alias = [pipeline_collect](
            const std::string& feature_name,
            const httplib::Request&, httplib::Response& res) {
        res.set_header("Deprecation", "true");
        res.set_header("Sunset", "2026-09-01");
        res.set_header("Link",
            "</api/v1/features/" + feature_name + "/collect>; rel=\"successor-version\"");
        pipeline_collect(feature_name, res);
    };

    srv.Get("/api/v1/cpu/utilization",
            [deprecated_alias](const httplib::Request& req, httplib::Response& res) {
                deprecated_alias("cpu_utilization", req, res);
            });
    srv.Get("/api/v1/cpu/processes",
            [deprecated_alias](const httplib::Request& req, httplib::Response& res) {
                deprecated_alias("cpu_processes", req, res);
            });
    srv.Get("/api/v1/cpu/profile/flamegraph",
            [deprecated_alias](const httplib::Request& req, httplib::Response& res) {
                deprecated_alias("cpu_profile", req, res);
            });
    srv.Get("/api/v1/cpu/profile/offcpu",
            [deprecated_alias](const httplib::Request& req, httplib::Response& res) {
                deprecated_alias("offcpu_profile", req, res);
            });
    srv.Get("/api/v1/cpu/sched/summary",
            [deprecated_alias](const httplib::Request& req, httplib::Response& res) {
                deprecated_alias("sched_analysis", req, res);
            });

    // QueryExtra endpoints (deprecated — prefer /api/v1/features/:name/collect for standard data)
    auto query_handler = [&controller](const std::string& pipeline_name,
                                        const std::string& query_name,
                                        const httplib::Request& req,
                                        httplib::Response& res) {
        res.set_header("Deprecation", "true");
        res.set_header("Sunset", "2026-09-01");
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
                        {"tier", static_cast<int>(f.tier)},
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
                 FeatureManager::StartParams params;

                 // Parse optional target_pids and target_comms from request body
                 if (!req.body.empty()) {
                     try {
                         auto body = json::parse(req.body);
                         if (body.contains("target_pids")) {
                             if (body["target_pids"].is_array()) {
                                 for (const auto& p : body["target_pids"])
                                     params.target_pids.push_back(p.get<uint32_t>());
                             } else if (body["target_pids"].is_number()) {
                                 params.target_pids.push_back(
                                     body["target_pids"].get<uint32_t>());
                             }
                         }
                         if (body.contains("target_comms")) {
                             if (body["target_comms"].is_array()) {
                                 for (const auto& c : body["target_comms"])
                                     params.target_comms.push_back(c.get<std::string>());
                             } else if (body["target_comms"].is_string()) {
                                 params.target_comms.push_back(
                                     body["target_comms"].get<std::string>());
                             }
                         }
                     } catch (...) {
                         // Non-JSON body is acceptable (backward compat)
                     }
                 }

                 auto status = features.Start(name, params);
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

    // Runtime filter reconfiguration (update target_pids without restart)
    srv.Post("/api/v1/features/:name/reconfigure",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 if (req.body.empty()) {
                     JsonError(res, "request body required", 400);
                     return;
                 }
                 FeatureManager::StartParams params;
                 try {
                     auto body = json::parse(req.body);
                     if (body.contains("target_pids")) {
                         if (body["target_pids"].is_array()) {
                             for (const auto& p : body["target_pids"])
                                 params.target_pids.push_back(p.get<uint32_t>());
                         } else if (body["target_pids"].is_number()) {
                             params.target_pids.push_back(
                                 body["target_pids"].get<uint32_t>());
                         }
                     }
                     if (body.contains("target_comms")) {
                         if (body["target_comms"].is_array()) {
                             for (const auto& c : body["target_comms"])
                                 params.target_comms.push_back(c.get<std::string>());
                         } else if (body["target_comms"].is_string()) {
                             params.target_comms.push_back(
                                 body["target_comms"].get<std::string>());
                         }
                     }
                 } catch (const std::exception& e) {
                     JsonError(res, std::string("invalid JSON: ") + e.what(), 400);
                     return;
                 }

                 auto status = features.ReconfigureFilter(name, params);
                 if (!status.ok()) {
                     int code = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                     JsonError(res, status.message(), code);
                     return;
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"message", "filter updated"}}.dump() + "\n",
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

    // === Resource Budget API ===
    srv.Get("/api/v1/budget",
            [&features](const httplib::Request&, httplib::Response& res) {
                auto& limiter = ResourceLimiter::Instance();
                auto usage = limiter.Check();

                auto list = features.ListFeatures();
                int active_count = 0;
                int ebpf_probes = 0;
                for (const auto& f : list) {
                    if (f.state == FeatureState::kActive ||
                        f.state == FeatureState::kPaused) {
                        active_count++;
                        if (f.tier == FeatureTier::kTracing ||
                            f.tier == FeatureTier::kProfiling) {
                            ebpf_probes++;
                        }
                    }
                }

                json j;
                j["usage"] = {
                    {"rss_bytes", usage.rss_bytes},
                    {"cpu_pct", usage.cpu_percent},
                    {"active_features", active_count},
                    {"ebpf_probes", ebpf_probes},
                };
                j["limits"] = {
                    {"max_memory_bytes", limiter.GetLimits().max_memory_bytes},
                    {"max_cpu_pct", limiter.GetLimits().max_cpu_percent},
                    {"max_ebpf_probes", 8},
                };
                j["exceeded"] = usage.memory_exceeded;
                res.set_content(j.dump() + "\n", "application/json");
            });

    // === Global Recording API ===
    srv.Post("/api/v1/recording/start",
             [&features](const httplib::Request&, httplib::Response& res) {
                 auto list = features.ListFeatures();
                 json started = json::array();
                 json errors = json::array();
                 for (const auto& f : list) {
                     if (f.state != FeatureState::kActive) continue;
                     auto sink = RecordingSinkRegistry::Instance().Get(f.name);
                     if (!sink) continue;
                     if (sink->IsRecording()) {
                         started.push_back(f.name);
                         continue;
                     }
                     auto st = sink->StartRecording();
                     if (st.ok()) started.push_back(f.name);
                     else errors.push_back({{"feature", f.name}, {"error", st.message()}});
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"recording_features", started},
                          {"errors", errors}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/recording/stop",
             [&features](const httplib::Request&, httplib::Response& res) {
                 auto list = features.ListFeatures();
                 json stopped = json::array();
                 for (const auto& f : list) {
                     auto sink = RecordingSinkRegistry::Instance().Get(f.name);
                     if (!sink || !sink->IsRecording()) continue;
                     sink->StopRecording();
                     auto session = sink->GetSession();
                     stopped.push_back({
                         {"feature", f.name},
                         {"file", session.file_path},
                         {"bytes", session.bytes_written},
                     });
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"stopped_features", stopped}}.dump() + "\n",
                     "application/json");
             });

    srv.Get("/api/v1/recording/status",
            [&features](const httplib::Request&, httplib::Response& res) {
                auto list = features.ListFeatures();
                bool any_recording = false;
                json recording_features = json::array();
                uint64_t total_bytes = 0;
                for (const auto& f : list) {
                    auto sink = RecordingSinkRegistry::Instance().Get(f.name);
                    if (!sink || !sink->IsRecording()) continue;
                    any_recording = true;
                    auto session = sink->GetSession();
                    recording_features.push_back({
                        {"feature", f.name},
                        {"bytes_written", session.bytes_written},
                    });
                    total_bytes += session.bytes_written;
                }
                res.set_content(
                    json{{"recording", any_recording},
                         {"features", recording_features},
                         {"total_bytes", total_bytes}}.dump() + "\n",
                    "application/json");
            });

    // === Plugin Hot-Reload API ===
    srv.Post("/api/v1/plugins/reload",
             [](const httplib::Request&, httplib::Response& res) {
                 auto& mgr = PluginManager::Instance();
                 auto& reg = PluginRegistry::Instance();

                 size_t before = reg.ListSources().size() + reg.ListProcessors().size()
                              + reg.ListAggregators().size() + reg.ListSinks().size();

                 auto status = mgr.LoadPluginsFromDirs(mgr.Loader().AllowedDirs());
                 if (!status.ok()) {
                     JsonError(res, "Plugin reload failed: " + status.message());
                     return;
                 }

                 size_t after = reg.ListSources().size() + reg.ListProcessors().size()
                             + reg.ListAggregators().size() + reg.ListSinks().size();

                 json plugins = json::array();
                 for (auto& n : reg.ListSources())     plugins.push_back({{"name", n}, {"type", "source"}});
                 for (auto& n : reg.ListProcessors())  plugins.push_back({{"name", n}, {"type", "processor"}});
                 for (auto& n : reg.ListAggregators()) plugins.push_back({{"name", n}, {"type", "aggregator"}});
                 for (auto& n : reg.ListSinks())       plugins.push_back({{"name", n}, {"type", "sink"}});

                 res.set_content(
                     json{{"status", "ok"},
                          {"loaded", after - before},
                          {"total", after},
                          {"plugins", plugins}}.dump() + "\n",
                     "application/json");
             });

    srv.Get("/api/v1/plugins",
            [](const httplib::Request&, httplib::Response& res) {
                auto& reg = PluginRegistry::Instance();
                json plugins = json::array();
                for (auto& n : reg.ListSources())     plugins.push_back({{"name", n}, {"type", "source"}, {"source", "builtin"}});
                for (auto& n : reg.ListProcessors())  plugins.push_back({{"name", n}, {"type", "processor"}, {"source", "builtin"}});
                for (auto& n : reg.ListAggregators()) plugins.push_back({{"name", n}, {"type", "aggregator"}, {"source", "builtin"}});
                for (auto& n : reg.ListSinks())       plugins.push_back({{"name", n}, {"type", "sink"}, {"source", "builtin"}});

                auto& mgr = PluginManager::Instance();
                for (auto& desc : mgr.Loader().Descriptors()) {
                    if (desc && desc->name) {
                        for (auto& p : plugins) {
                            if (p["name"] == desc->name) {
                                p["source"] = "shared_object";
                                break;
                            }
                        }
                    }
                }

                res.set_content(
                    json{{"plugins", plugins}}.dump() + "\n",
                    "application/json");
            });
}

}  // namespace illuminator
