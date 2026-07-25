// ============================================================================
// Illuminator HTTP API 路由注册
// ============================================================================
//
// 【架构定位】
// 本文件将所有 REST API 端点注册到 httplib::Server，是 Server 层的核心组件。
// 拆分自 main.cc，职责单一化：API 路由注册与请求处理。
//
// 【文件结构】
//   1. 基础工具函数（JsonError、SetupAuthMiddleware）
//   2. RegisterApiRoutes() — 核心 API 路由
//      - /healthz:                健康检查（无需认证）
//      - /api/v1/pipelines:       管道列表（FeatureBus 管理的管道）
//      - /api/v1/pipelines/:name/collect: 同步采集（调试用）
//      - /api/v1/channel_stats:   通道统计（所有管道的 AsyncChannel 指标）
//      - /metrics:                Prometheus 格式指标
//      - /api/v1/internal_metrics: JSON 格式内部指标
//   3. 录制 API
//      - /api/v1/features/:name/record/start: 开始录制
//      - /api/v1/features/:name/record/stop:  停止录制
//      - /api/v1/features/:name/record/status: 录制状态
//   4. 全局录制 API
//      - /api/v1/recording/start: 全局开始录制
//      - /api/v1/recording/stop:  全局停止录制
//      - /api/v1/recording/status: 全局录制状态
//   5. 插件 API
//      - /api/v1/plugins/reload: 插件热重载
//      - /api/v1/plugins:        插件列表
//
// 【认证机制】
//   所有 /api/ 路径的请求都需要 Bearer Token 认证（通过 SetupAuthMiddleware 设置）。
//   /healthz 和 /metrics 不需要认证。
//
// 【API 版本演进】
//   v1 路由保留 pipelines/collect/channel_stats/metrics 等核心运维端点。
//   Feature 管理通过 /api/v2/features/* 路由（定义在 api_v2_routes.h 中）。
// ============================================================================

#pragma once

#include <algorithm>
#include <mutex>
#include <string>

#include "httplib.h"

#include "core/engine/self_observability.h"
#include "core/common/version_generated.h"
#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "plugin/infra/plugin_manager.h"
#include "core/common/json_serializer.h"
#include "plugin/api/recording_interface.h"

namespace illuminator {

static constexpr const char* kIlluminatorVersion = kBuildVersion;

// ============================================================================
// JsonError — 统一 JSON 错误响应
// ============================================================================
// 设置 HTTP 状态码和 JSON 格式的错误响应体。
//
// 响应格式：{"error": "<msg>"}
//
// 参数：
//   res:    httplib::Response 引用
//   msg:    错误消息
//   status: HTTP 状态码（默认 500）
inline void JsonError(httplib::Response& res, const std::string& msg,
                      int status = 500) {
    res.status = status;
    res.set_content(json{{"error", msg}}.dump() + "\n", "application/json");
}

// ============================================================================
// SetupAuthMiddleware — 设置认证中间件
// ============================================================================
//
// 拦截所有 /api/ 路径的请求，验证 Authorization: Bearer <token> 头。
// /healthz 和 /metrics 不需要认证（用于健康检查和 Prometheus 抓取）。
//
// 如果 auth_token 为空，表示不需要认证，中间件不生效。
//
// 参数：
//   srv:        httplib::Server 引用
//   auth_token: 认证 Token（空字符串表示不需要认证）
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

            std::string expected = "Bearer " + auth_token;

            auto it = req.headers.find("Authorization");
            if (it != req.headers.end() && it->second == expected) {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            if (req.has_param("token") && req.get_param_value("token") == auth_token) {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            if (it == req.headers.end()) {
                res.status = 401;
                res.set_content(
                    R"json({"error":"missing Authorization header (or ?token= query param)"})json",
                    "application/json");
            } else {
                res.status = 403;
                res.set_content(R"({"error":"invalid token"})",
                                "application/json");
            }
            return httplib::Server::HandlerResponse::Handled;
        });
}

// ============================================================================
// RegisterApiRoutes — 注册核心 API 路由
// ============================================================================
//
// 注册 Illuminator 的核心 API 端点。所有管道状态通过 FeatureBus 查询。
// 所有管道状态通过 FeatureBus 查询（RFC v3）。
inline void RegisterApiRoutes(httplib::Server& srv) {
    // ========================================================================
    // /healthz — 健康检查（无需认证）
    // ========================================================================
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        json j;
        j["status"] = "ok";
        j["version"] = kIlluminatorVersion;
        j["commit"] = kBuildCommit;
        res.set_content(j.dump() + "\n", "application/json");
    });

    // ========================================================================
    // /api/v1/pipelines — 管道列表 (通过 FeatureBus 查询)
    // ========================================================================
    srv.Get("/api/v1/pipelines",
            [](const httplib::Request&, httplib::Response& res) {
                auto& bus = FeatureBus::Instance();
                auto drivers = bus.ListDrivers();
                json arr = json::array();
                for (auto& info : drivers) {
                    auto* drv = bus.GetDriver(info.name);
                    Pipeline* pipeline = drv ? drv->GetPipeline() : nullptr;
                    json entry;
                    entry["name"] = info.name;
                    entry["running"] = (info.state == DriverState::kActive);
                    entry["origin"] = "feature_bus";
                    entry["batches"] = info.batches_processed;
                    entry["records"] = info.records_processed;
                    if (pipeline) {
                        entry["channel"] = {
                            {"capacity", pipeline->ChannelCapacity()},
                            {"size", pipeline->ChannelSize()},
                            {"enqueued", pipeline->ChannelEnqueued()},
                            {"dequeued", pipeline->ChannelDequeued()},
                            {"dropped", pipeline->ChannelDropped()},
                            {"flush_injected", pipeline->ChannelFlushInjected()},
                            {"backpressure_events", pipeline->ChannelBackpressureEvents()},
                            {"backpressured", pipeline->ChannelBackpressured()},
                        };
                    }
                    arr.push_back(std::move(entry));
                }
                res.set_content(
                    json{{"pipelines", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });

    // ========================================================================
    // /api/v1/pipelines/:name/collect — 同步采集（通过 FeatureBus）
    // ========================================================================
    srv.Get("/api/v1/pipelines/:name/collect",
            [](const httplib::Request& req, httplib::Response& res) {
                auto name = req.path_params.at("name");
                auto& bus = FeatureBus::Instance();
                auto* drv = bus.GetDriver(name);
                if (!drv || !drv->GetPipeline()) {
                    JsonError(res, "feature '" + name + "' not found or inactive", 404);
                    return;
                }
                auto* source = drv->GetPipeline()->GetSource();
                if (!source) {
                    JsonError(res, "feature '" + name + "' has no source");
                    return;
                }
                auto result = source->Collect();
                if (!result.ok()) {
                    JsonError(res, result.status().message());
                    return;
                }
                auto processed = drv->GetPipeline()->RunProcessors(std::move(*result));
                if (!processed.ok()) {
                    JsonError(res, processed.status().message());
                    return;
                }
                res.set_content(BatchToJson(**processed, name) + "\n",
                                "application/json");
            });

    // ========================================================================
    // /api/v1/channel_stats — 通道统计（通过 FeatureBus 获取）
    // ========================================================================
    srv.Get("/api/v1/channel_stats",
            [](const httplib::Request&, httplib::Response& res) {
                auto& bus = FeatureBus::Instance();
                auto drivers = bus.ListDrivers();
                json arr = json::array();
                for (auto& info : drivers) {
                    auto* drv = bus.GetDriver(info.name);
                    Pipeline* pipeline = drv ? drv->GetPipeline() : nullptr;
                    if (!pipeline) continue;
                    arr.push_back({
                        {"pipeline", info.name},
                        {"capacity", pipeline->ChannelCapacity()},
                        {"size", pipeline->ChannelSize()},
                        {"utilization", pipeline->ChannelCapacity() > 0
                            ? static_cast<double>(pipeline->ChannelSize()) / pipeline->ChannelCapacity()
                            : 0.0},
                        {"enqueued", pipeline->ChannelEnqueued()},
                        {"dequeued", pipeline->ChannelDequeued()},
                        {"dropped", pipeline->ChannelDropped()},
                        {"flush_injected", pipeline->ChannelFlushInjected()},
                        {"backpressure_events", pipeline->ChannelBackpressureEvents()},
                        {"backpressured", pipeline->ChannelBackpressured()},
                    });
                }
                res.set_content(
                    json{{"channels", std::move(arr)}}.dump() + "\n",
                    "application/json");
            });


    // ========================================================================
    // /metrics — Prometheus 格式指标（无需认证）
    // ========================================================================
    // 返回 Prometheus 文本格式的指标数据，可供 Prometheus 直接抓取。
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(InternalMetrics::Instance().ExportPrometheus(),
                        "text/plain");
    });

    // ========================================================================
    // /api/v1/internal_metrics — JSON 格式内部指标
    // ========================================================================
    // 返回 JSON 格式的内部指标数据，用于调试和监控。
    srv.Get("/api/v1/internal_metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(InternalMetrics::Instance().ExportJson() + "\n",
                        "application/json");
    });

    // ========================================================================
    // 录制 API（开始 / 停止 / 状态）
    // ========================================================================
    srv.Post("/api/v1/features/:name/record/start",
             [](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto& bus = FeatureBus::Instance();
                 auto* drv = bus.GetDriver(name);
                 if (!drv) {
                     JsonError(res, "Feature not found: " + name, 404);
                     return;
                 }
                 std::string output_dir = "/tmp/illuminator_data";
                 try {
                     auto j = json::parse(req.body);
                     if (j.contains("output_dir") && j["output_dir"].is_string()) {
                         output_dir = j["output_dir"];
                     }
                 } catch (...) {
                     // 解析失败时使用默认目录
                 }
                 auto status = drv->StartRecording(output_dir);
                 if (!status.ok()) {
                     JsonError(res, status.message(), 400);
                     return;
                 }
                 auto sink = RecordingSinkRegistry::Instance().Get(name);
                 auto session = sink ? sink->GetSession() : RecordingSession{};
                 res.set_content(
                     json{{"status", "ok"}, {"feature", name},
                          {"file", session.file_path}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/features/:name/record/stop",
             [](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 auto& bus = FeatureBus::Instance();
                 auto* drv = bus.GetDriver(name);
                 if (!drv) {
                     JsonError(res, "Feature not found: " + name, 404);
                     return;
                 }
                 auto sink = RecordingSinkRegistry::Instance().Get(name);
                 auto session = sink ? sink->GetSession() : RecordingSession{};
                 auto status = drv->StopRecording();
                 if (!status.ok()) {
                     JsonError(res, status.message(), 400);
                     return;
                 }
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
                auto& bus = FeatureBus::Instance();
                auto* drv = bus.GetDriver(name);
                if (!drv || !drv->IsRecording()) {
                    res.set_content(
                        json{{"feature", name}, {"recording", false}}.dump() + "\n",
                        "application/json");
                    return;
                }
                auto sink = RecordingSinkRegistry::Instance().Get(name);
                auto session = sink ? sink->GetSession() : RecordingSession{};
                json j;
                j["feature"] = name;
                j["recording"] = true;
                j["file"] = session.file_path;
                j["bytes_written"] = session.bytes_written;
                j["batches_written"] = session.batches_written;
                res.set_content(j.dump() + "\n", "application/json");
            });

    // ========================================================================
    // 全局录制 API
    // ========================================================================
    srv.Post("/api/v1/recording/start",
             [](const httplib::Request& req, httplib::Response& res) {
                 std::string output_dir = "/tmp/illuminator_data";
                 try {
                     auto j = json::parse(req.body);
                     if (j.contains("output_dir") && j["output_dir"].is_string()) {
                         output_dir = j["output_dir"];
                     }
                 } catch (...) {
                     // 解析失败时使用默认目录
                 }
                 auto& bus = FeatureBus::Instance();
                 json started = json::array();
                 json errors = json::array();
                 for (auto& info : bus.ListDrivers()) {
                     auto* drv = bus.GetDriver(info.name);
                     if (!drv) continue;
                     if (drv->IsRecording()) {
                         started.push_back(info.name);
                         continue;
                     }
                     auto st = drv->StartRecording(output_dir);
                     if (st.ok()) started.push_back(info.name);
                     else errors.push_back({{"feature", info.name}, {"error", st.message()}});
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"recording_features", started},
                          {"errors", errors}}.dump() + "\n",
                     "application/json");
             });

    srv.Post("/api/v1/recording/stop",
             [](const httplib::Request&, httplib::Response& res) {
                 auto& bus = FeatureBus::Instance();
                 auto& registry = RecordingSinkRegistry::Instance();
                 json stopped = json::array();
                 for (const auto& name : registry.ListNames()) {
                     auto sink = registry.Get(name);
                     if (!sink || !sink->IsRecording()) continue;
                     auto session = sink->GetSession();
                     auto* drv = bus.GetDriver(name);
                     if (!drv) continue;
                     auto st = drv->StopRecording();
                     if (!st.ok()) continue;
                     stopped.push_back({
                         {"feature", name},
                         {"file", session.file_path},
                         {"bytes", session.bytes_written},
                     });
                 }
                 res.set_content(
                     json{{"status", "ok"}, {"stopped_features", stopped}}.dump() + "\n",
                     "application/json");
             });

    srv.Get("/api/v1/recording/status",
            [](const httplib::Request&, httplib::Response& res) {
                auto& bus = FeatureBus::Instance();
                bool any_recording = false;
                json recording_features = json::array();
                uint64_t total_bytes = 0;
                for (auto& info : bus.ListDrivers()) {
                    auto* drv = bus.GetDriver(info.name);
                    if (!drv || !drv->IsRecording()) continue;
                    any_recording = true;
                    auto sink = RecordingSinkRegistry::Instance().Get(info.name);
                    auto session = sink ? sink->GetSession() : RecordingSession{};
                    recording_features.push_back({
                        {"feature", info.name},
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

    // ========================================================================
    // 插件 API
    // ========================================================================
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

    // /api/v1/query 和 /api/v1/budget 端点待实现（需要完善 storage deps）
}


}  // namespace illuminator