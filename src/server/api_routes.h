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

// JsonError — 统一 JSON 错误响应
inline void JsonError(httplib::Response& res, const std::string& msg,
                      int status = 500) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump() + "\n", "application/json");
}

// SetupAuthMiddleware — 设置认证中间件
// 拦截所有 /api/ 路径的请求，验证 Authorization: Bearer <token> 头。
// /healthz 和 /metrics 不需要认证。
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

// RegisterApiRoutes — 注册核心 API 路由
// 包括：/healthz、/api/v1/pipelines、/api/v1/pipelines/:name/collect、
//       /api/v1/channel_stats、/api/v1/query、/metrics 等
inline void RegisterApiRoutes(httplib::Server& srv,
                              PipelineController& controller,
                              FeatureManager* features = nullptr) {
    // ---- 健康检查 ----
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status", "ok"}, {"version", kIlluminatorVersion},
                 {"commit", kBuildCommit}}.dump() + "\n",
            "application/json");
    });

    // ---- 管道列表（包含 Controller 和 FeatureManager 管理的管道） ----
    srv.Get("/api/v1/pipelines",
            [&controller, features](const httplib::Request&, httplib::Response& res) {
                json arr = json::array();
                for (auto& p : controller.Pipelines()) {
                    auto* src = p->GetSource();
                    arr.push_back({
                        {"name", p->name()},
                        {"running", p->IsRunning()},
                        {"origin", "controller"},
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

                // 包含 FeatureManager 管理的活跃 Feature 管道
                // Include active feature-managed pipelines
                json active_arr = json::array();
                if (features) {
                    for (const auto& f : features->ListFeatures()) {
                        if (f.state == FeatureState::kActive || f.state == FeatureState::kPaused) {
                            active_arr.push_back({
                                {"name", f.name},
                                {"display_name", f.display_name},
                                {"category", f.category},
                                {"state", FeatureStateToString(f.state)},
                                {"origin", "feature_manager"},
                                {"running", f.state == FeatureState::kActive},
                                {"batches", f.batches_processed},
                                {"records", f.records_processed},
                                {"errors", f.errors},
                                {"uptime_ms", f.uptime_ms},
                            });
                        }
                    }
                }

                res.set_content(
                    json{{"pipelines", std::move(arr)},
                         {"active_features", std::move(active_arr)}}.dump() + "\n",
                    "application/json");
            });

    // ---- 管道数据采集（同步阻塞，已废弃，推荐使用 /api/v1/features/:name/collect） ----
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

    // ---- 已废弃的旧 API 端点（重定向到 feature API） ----
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

    // ---- 已废弃：旧 QueryExtra 端点（推荐使用 /api/v1/features/:name/collect） ----
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

    // ---- Channel 统计端点（所有管道的通道指标） ----
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

    // ---- SQL 查询端点（只读，仅允许 SELECT 和 PRAGMA） ----
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

    // ---- 指标端点（Prometheus 格式和 JSON 格式） ----
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
// Feature API — 按需启停 + 实时流控制 + 录制
// ============================================================================
// 提供 Feature 的完整生命周期管理 API：
//   - /api/v1/features               — 列出所有 Feature
//   - /api/v1/features/:name/start   — 启动 Feature
//   - /api/v1/features/:name/stop    — 停止 Feature
//   - /api/v1/features/:name/pause   — 暂停 Feature
//   - /api/v1/features/:name/resume  — 恢复 Feature
//   - /api/v1/features/:name/reconfigure — 在线更新过滤条件
//   - /api/v1/features/:name/collect — 拉取最新数据
//   - /api/v1/features/:name/stream  — 增量数据拉取（支持 cursor）
//   - /api/v1/features/:name/record/start — 开始录制
//   - /api/v1/features/:name/record/stop  — 停止录制
//   - /api/v1/features/:name/record/status — 录制状态
//   - /api/v1/budget                  — 资源预算查询
//   - /api/v1/recording/start         — 全局开始录制
//   - /api/v1/recording/stop          — 全局停止录制
//   - /api/v1/recording/status        — 全局录制状态
//   - /api/v1/plugins/reload          — 插件热重载
//   - /api/v1/plugins                 — 插件列表
inline void RegisterFeatureRoutes(httplib::Server& srv,
                                   FeatureManager& features) {
    // ---- 列出所有 Feature ----
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

    // ---- 启动 Feature ----
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

    // ---- 在线重配置过滤条件（不重启 Pipeline） ----
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

    // ---- 拉取最新数据（从 StreamSinkStore 读取，不干扰 Pipeline 采集） ----
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

    // ---- 增量数据拉取（支持 cursor 参数避免重复消费） ----
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

    // ---- 录制 API（开始 / 停止 / 状态） ----
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

    // ---- 资源预算查询（当前资源使用 vs 限制） ----
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

    // === Export API (replaces global recording with lookback support) ===
    // POST /api/v1/export — export recent data from ring buffers + continue capturing
    srv.Post("/api/v1/export",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 json body;
                 try { body = json::parse(req.body); }
                 catch (...) { JsonError(res, "Invalid JSON body", 400); return; }

                 std::vector<std::string> target_features;
                 if (body.contains("features") && body["features"].is_array()) {
                     for (auto& f : body["features"])
                         target_features.push_back(f.get<std::string>());
                 } else {
                     for (const auto& f : features.ListFeatures())
                         if (f.state == FeatureState::kActive)
                             target_features.push_back(f.name);
                 }

                 size_t lookback = body.value("lookback_batches", 60);

                 auto now = std::chrono::system_clock::now();
                 auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch()).count();
                 std::string filename = "export_" + std::to_string(epoch_ms) + ".ilr";
                 std::string output_dir = "/tmp/illuminator_exports";
                 (void)std::system(("mkdir -p " + output_dir).c_str());
                 std::string path = output_dir + "/" + filename;

                 std::ofstream out(path, std::ios::binary);
                 if (!out.is_open()) {
                     JsonError(res, "Cannot create export file: " + path, 500);
                     return;
                 }

                 json header;
                 header["format"] = "ilr";
                 header["version"] = 1;
                 header["type"] = "export";
                 header["features"] = target_features;
                 header["started_at"] = epoch_ms;
                 header["lookback_batches"] = lookback;
                 out << header.dump() << "\n";

                 size_t total_batches = 0;
                 auto& store = StreamSinkStore::Instance();
                 for (const auto& fname : target_features) {
                     auto& buf = store.GetBuffer(fname);
                     auto recent = buf.Recent(lookback);
                     for (const auto& batch : recent) {
                         if (!batch) continue;
                         json j;
                         j["feature"] = fname;
                         j["ts"] = epoch_ms;
                         j["data"] = json::parse(BatchToJson(*batch, fname));
                         out << j.dump() << "\n";
                         ++total_batches;
                     }
                 }
                 out.close();

                 res.set_content(
                     json{{"status", "ok"},
                          {"file", path},
                          {"features_exported", target_features.size()},
                          {"batches_exported", total_batches}}.dump() + "\n",
                     "application/json");
             });

    // === Session API (for Tier 3 profiling with auto-expiry) ===
    // POST /api/v1/sessions — create a profiling session
    srv.Post("/api/v1/sessions",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 json body;
                 try { body = json::parse(req.body); }
                 catch (...) { JsonError(res, "Invalid JSON body", 400); return; }

                 std::string type = body.value("type", "");
                 if (type.empty()) {
                     JsonError(res, "Missing 'type' field (e.g. cpu_profile, offcpu_profile)", 400);
                     return;
                 }

                 FeatureManager::StartParams params;
                 if (body.contains("target_pids")) {
                     for (auto& p : body["target_pids"])
                         params.target_pids.push_back(p.get<int>());
                 }
                 if (body.contains("target_comms")) {
                     for (auto& c : body["target_comms"])
                         params.target_comms.push_back(c.get<std::string>());
                 }

                 int duration_sec = body.value("duration_sec", 30);

                 auto status = features.Start(type, params);
                 if (!status.ok()) {
                     JsonError(res, "Failed to start session: " + status.message(), 400);
                     return;
                 }

                 auto now = std::chrono::system_clock::now();
                 auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch()).count();
                 std::string session_id = type + "_" + std::to_string(epoch_ms);

                 json resp;
                 resp["session_id"] = session_id;
                 resp["type"] = type;
                 resp["status"] = "active";
                 resp["started_at"] = epoch_ms;
                 if (duration_sec > 0) {
                     resp["expires_at"] = epoch_ms + duration_sec * 1000;
                     resp["duration_sec"] = duration_sec;
                 }
                 res.set_content(resp.dump() + "\n", "application/json");
             });

    // POST /api/v1/sessions/stop — stop a profiling session by feature type
    srv.Post("/api/v1/sessions/stop",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 json body;
                 try { body = json::parse(req.body); }
                 catch (...) { JsonError(res, "Invalid JSON body", 400); return; }

                 std::string type = body.value("type", "");
                 if (type.empty()) {
                     JsonError(res, "Missing 'type' field", 400);
                     return;
                 }

                 auto status = features.Stop(type);
                 if (!status.ok()) {
                     JsonError(res, "Failed to stop session: " + status.message(), 400);
                     return;
                 }

                 res.set_content(
                     json{{"status", "ok"}, {"type", type}, {"stopped", true}}.dump() + "\n",
                     "application/json");
             });

    // GET /api/v1/sessions — list active profiling sessions (Tier 3 features)
    srv.Get("/api/v1/sessions",
            [&features](const httplib::Request&, httplib::Response& res) {
                auto list = features.ListFeatures();
                json sessions = json::array();
                for (const auto& f : list) {
                    if (static_cast<int>(f.tier) >= 3 && f.state == FeatureState::kActive) {
                        sessions.push_back({
                            {"type", f.name},
                            {"category", f.category},
                            {"status", "active"},
                            {"uptime_ms", f.uptime_ms},
                        });
                    }
                }
                res.set_content(
                    json{{"sessions", sessions}}.dump() + "\n",
                    "application/json");
            });
}

}  // namespace illuminator
