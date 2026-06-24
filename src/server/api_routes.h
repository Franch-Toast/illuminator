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
//      - /api/v1/pipelines:       管道列表（含 Controller 和 FeatureManager 管理的管道）
//      - /api/v1/pipelines/:name/collect: 管道数据采集（已废弃，推荐使用 Feature API）
//      - /api/v1/channel_stats:   通道统计（所有管道的 AsyncChannel 指标）
//      - /api/v1/query:           SQL 查询（只读，仅允许 SELECT 和 PRAGMA）
//      - /metrics:                Prometheus 格式指标
//      - /api/v1/internal_metrics: JSON 格式内部指标
//   3. RegisterFeatureRoutes() — Feature API 路由
//      - /api/v1/features:               列出所有 Feature
//      - /api/v1/features/:name/start:   启动 Feature
//      - /api/v1/features/:name/stop:    停止 Feature
//      - /api/v1/features/:name/pause:   暂停 Feature
//      - /api/v1/features/:name/resume:  恢复 Feature
//      - /api/v1/features/:name/reconfigure: 在线更新过滤条件
//      - /api/v1/features/:name/collect: 拉取最新数据
//      - /api/v1/features/:name/stream:  增量数据拉取（支持 cursor）
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
//   6. 资源预算 API
//      - /api/v1/budget: 资源预算查询
//   7. Session API（Tier 3 profiling 会话管理）
//      - /api/v1/sessions:       创建/列出 profiling 会话
//      - /api/v1/sessions/stop:  停止 profiling 会话
//   8. Export API
//      - /api/v1/export: 导出环形缓冲区数据为 .ilr 文件
//
// 【认证机制】
//   所有 /api/ 路径的请求都需要 Bearer Token 认证（通过 SetupAuthMiddleware 设置）。
//   /healthz 和 /metrics 不需要认证。
//
// 【API 版本演进】
//   旧版 API（/api/v1/cpu/*、/api/v1/pipelines/:name/collect）已标记为
//   Deprecated，推荐使用 Feature API（/api/v1/features/*）。
//   旧 API 返回 Deprecation 和 Sunset 头，提示客户端迁移。
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
    if (auth_token.empty()) return;  // 不需要认证

    // 使用 pre_routing_handler 在所有路由处理之前拦截请求
    srv.set_pre_routing_handler(
        [auth_token](const httplib::Request& req, httplib::Response& res) {
            // 健康检查和指标端点不需要认证
            if (req.path == "/healthz" || req.path == "/metrics") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            // 非 API 路径不需要认证（如静态文件）
            if (req.path.find("/api/") != 0) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            // 验证 Authorization 头
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

// ============================================================================
// RegisterApiRoutes — 注册核心 API 路由
// ============================================================================
//
// 注册 Illuminator 的核心 API 端点，包括健康检查、管道状态、数据采集、
// 通道统计、SQL 查询和指标等。
//
// 参数：
//   srv:        httplib::Server 引用
//   controller: PipelineController 引用（用于查询管道状态和采集数据）
//   features:   FeatureManager 指针（可选，用于包含活跃 Feature 管道信息）
inline void RegisterApiRoutes(httplib::Server& srv,
                              PipelineController& controller,
                              FeatureManager* features = nullptr) {
    // ========================================================================
    // /healthz — 健康检查（无需认证）
    // ========================================================================
    // 返回版本号、commit hash 和运行状态。用于 Kubernetes 健康检查探针。
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            json{{"status", "ok"}, {"version", kIlluminatorVersion},
                 {"commit", kBuildCommit}}.dump() + "\n",
            "application/json");
    });

    // ========================================================================
    // /api/v1/pipelines — 管道列表
    // ========================================================================
    // 返回所有管道的状态信息，包括：
    //   - PipelineController 管理的管道（来自 YAML 配置）
    //   - FeatureManager 管理的活跃 Feature 管道（通过 features 参数）
    //
    // 响应格式：
    //   {
    //     "pipelines": [...],         // PipelineController 管理的管道
    //     "active_features": [...]    // FeatureManager 管理的活跃 Feature
    //   }
    srv.Get("/api/v1/pipelines",
            [&controller, features](const httplib::Request&, httplib::Response& res) {
                json arr = json::array();
                // PipelineController 管理的管道
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

                // FeatureManager 管理的活跃 Feature 管道
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

    // ========================================================================
    // /api/v1/pipelines/:name/collect — 管道数据采集（已废弃）
    // ========================================================================
    // 同步阻塞式采集，直接调用 Source::Collect() 并返回 JSON 数据。
    // 已废弃，推荐使用 /api/v1/features/:name/collect（从 StreamSinkStore 读取，
    // 不干扰 Pipeline 正常采集）。
    //
    // 返回 Deprecation、Sunset 和 Link 头，提示客户端迁移到新 API。
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

    // ========================================================================
    // 已废弃的旧 API 端点（重定向到 feature API）
    // ========================================================================
    // 这些是早期版本的 API 端点，已迁移到 Feature API。
    // 保留这些端点以兼容旧客户端，但返回 Deprecation 头提示迁移。
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

    // ========================================================================
    // 已废弃的 QueryExtra 端点
    // ========================================================================
    // 推荐使用 /api/v1/features/:name/collect 获取标准数据。
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

    // Off-CPU 火焰图快照
    srv.Get("/api/v1/cpu/profile/offcpu/snapshot",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("offcpu_profile", "snapshot", req, res);
            });
    // On-CPU 火焰图快照
    srv.Get("/api/v1/cpu/profile/oncpu/snapshot",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("cpu_profile", "snapshot", req, res);
            });
    // 调度历史
    srv.Get("/api/v1/cpu/sched/history",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "history", req, res);
            });
    // 调度事件
    srv.Get("/api/v1/cpu/sched/events",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "events", req, res);
            });
    // 唤醒事件
    srv.Get("/api/v1/cpu/sched/wakeups",
            [query_handler](const httplib::Request& req, httplib::Response& res) {
                query_handler("sched_analysis", "wakeups", req, res);
            });

    // ========================================================================
    // /api/v1/channel_stats — 通道统计（所有管道的 AsyncChannel 指标）
    // ========================================================================
    // 返回每个管道的 AsyncChannel 统计信息，包括：
    //   - capacity: 通道容量
    //   - size: 当前队列大小
    //   - utilization: 利用率（size / capacity）
    //   - enqueued/dequeued/dropped: 入队/出队/丢包计数
    //   - flush_injected: 注入的 FlushSignal 计数
    //   - backpressure_events: 反压事件计数
    //   - backpressured: 当前是否处于反压状态
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

    // ========================================================================
    // /api/v1/query — SQL 查询端点（只读）
    // ========================================================================
    // 对存储后端执行只读 SQL 查询。仅允许 SELECT 和 PRAGMA 语句，
    // 防止 INSERT/UPDATE/DELETE/DROP 等危险操作。
    //
    // 请求体：
    //   {"query": "SELECT * FROM cpu_metrics WHERE timestamp > ..."}
    //
    // 安全机制：
    //   - 检查 SQL 前 20 个字符是否包含 SELECT 或 PRAGMA
    //   - 这是一种简单的安全检查，不是完整的 SQL 注入防护
    srv.Post("/api/v1/query",
            [&controller](const httplib::Request& req, httplib::Response& res) {
                try {
                    auto body = json::parse(req.body);
                    std::string sql = body.value("query", "");
                    if (sql.empty()) {
                        JsonError(res, "missing 'query' field", 400);
                        return;
                    }
                    // 安全检查：只允许 SELECT 和 PRAGMA 查询
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
}

// ============================================================================
// RegisterFeatureRoutes — Feature API 路由
// ============================================================================
//
// 提供 Feature 的完整生命周期管理 API，包括：
//   - 生命周期控制：start/stop/pause/resume
//   - 数据访问：collect（最新数据）、stream（增量拉取）
//   - 在线重配置：reconfigure（更新 target_pids 不重启）
//   - 录制控制：record/start/stop/status
//   - 资源预算：budget
//   - 全局录制：recording/start/stop/status
//   - 插件管理：plugins、plugins/reload
//   - Session 管理：sessions（Tier 3 profiling 会话）
//   - 导出：export（环形缓冲区数据导出为 .ilr 文件）
//
// 参数：
//   srv:      httplib::Server 引用
//   features: FeatureManager 引用
inline void RegisterFeatureRoutes(httplib::Server& srv,
                                   FeatureManager& features) {
    // ========================================================================
    // GET /api/v1/features — 列出所有 Feature
    // ========================================================================
    // 返回所有 Feature 的运行时状态快照，包括：
    //   - name, display_name, category, tier（静态信息）
    //   - state, is_recording（状态信息）
    //   - batches_processed, records_processed, errors（统计信息）
    //   - uptime_ms（运行时长）
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

    // ========================================================================
    // POST /api/v1/features/:name/start — 启动 Feature
    // ========================================================================
    // 请求体（可选）：
    //   {
    //     "target_pids": [1234, 5678],     // 目标进程 PID 列表
    //     "target_comms": ["nginx"]         // 目标进程名列表
    //   }
    //
    // 响应：
    //   {"status": "ok", "feature": "cpu_utilization", "state": "active"}
    srv.Post("/api/v1/features/:name/start",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 auto name = req.path_params.at("name");
                 FeatureManager::StartParams params;

                 // 从请求体中解析 target_pids 和 target_comms
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
                         // 非 JSON 请求体可接受（向后兼容）
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

    // ========================================================================
    // POST /api/v1/features/:name/stop — 停止 Feature
    // ========================================================================
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

    // ========================================================================
    // POST /api/v1/features/:name/pause — 暂停 Feature
    // ========================================================================
    // 暂停时取消 TimerWheel 定时器，Pipeline 线程保持运行。
    // 适用于用户切换标签页时暂停采集以节省资源。
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

    // ========================================================================
    // POST /api/v1/features/:name/resume — 恢复 Feature
    // ========================================================================
    // 重新注册 TimerWheel 定时器，恢复数据采集。
    // 相比 Stop+Start，Resume 是毫秒级的（不重建 Pipeline）。
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

    // ========================================================================
    // POST /api/v1/features/:name/reconfigure — 在线重配置过滤条件
    // ========================================================================
    // 更新 Source 的 target_pids / target_comms，不重启 Pipeline。
    // 适用于用户切换监控的目标进程时，无缝切换。
    //
    // 请求体：
    //   {
    //     "target_pids": [1234, 5678],
    //     "target_comms": ["nginx"]
    //   }
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

    // ========================================================================
    // GET /api/v1/features/:name/collect — 拉取最新数据
    // ========================================================================
    // 从 StreamSinkStore 读取最新数据，不干扰 Pipeline 正常采集。
    // 与已废弃的 /api/v1/pipelines/:name/collect 不同，这个端点不会触发
    // 新的 Source::Collect()，而是从缓存中读取。
    srv.Get("/api/v1/features/:name/collect",
            [&features](const httplib::Request& req, httplib::Response& res) {
                auto name = req.path_params.at("name");
                auto state = features.GetState(name);
                if (state == FeatureState::kInactive) {
                    JsonError(res, "Feature '" + name + "' is not active", 400);
                    return;
                }

                // 从 StreamSinkStore 拉取最新数据
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

    // ========================================================================
    // GET /api/v1/features/:name/stream — 增量数据拉取
    // ========================================================================
    // 支持 cursor 参数避免重复消费。客户端通过 cursor 告诉服务端
    // "我已经消费到第 N 条"，服务端只返回 N 之后的新数据。
    //
    // 查询参数：
    //   cursor: 上次消费的序号（可选，默认为 0）
    //
    // 响应格式：
    //   {
    //     "feature": "cpu_utilization",
    //     "cursor": 123,
    //     "batches": [...]
    //   }
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

    // ========================================================================
    // 录制 API（开始 / 停止 / 状态）
    // ========================================================================
    // 录制功能由 RecordingSink 提供，录制数据写入磁盘文件（.ilr 格式），
    // 可导出和回放。

    // POST /api/v1/features/:name/record/start — 开始录制
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

    // POST /api/v1/features/:name/record/stop — 停止录制
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

    // GET /api/v1/features/:name/record/status — 录制状态
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

    // ========================================================================
    // GET /api/v1/budget — 资源预算查询
    // ========================================================================
    // 返回当前资源使用 vs 限制的对比，包括：
    //   - 内存使用（RSS）
    //   - CPU 使用率
    //   - 活跃 Feature 数量
    //   - eBPF 探针数量（Tier 2 + Tier 3）
    //   - 资源限制
    //   - 是否超出限制
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

    // ========================================================================
    // 全局录制 API
    // ========================================================================

    // POST /api/v1/recording/start — 全局开始录制
    // 对所有活跃 Feature 启动录制。
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

    // POST /api/v1/recording/stop — 全局停止录制
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

    // GET /api/v1/recording/status — 全局录制状态
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

    // ========================================================================
    // 插件 API
    // ========================================================================

    // POST /api/v1/plugins/reload — 插件热重载
    // 重新扫描插件目录，加载新的 .so 插件文件。
    // 返回重载前后插件数量对比。
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

    // GET /api/v1/plugins — 插件列表
    // 返回所有已注册的插件（包括内置插件和 .so 外部插件），标注来源（builtin/shared_object）。
    srv.Get("/api/v1/plugins",
            [](const httplib::Request&, httplib::Response& res) {
                auto& reg = PluginRegistry::Instance();
                json plugins = json::array();
                for (auto& n : reg.ListSources())     plugins.push_back({{"name", n}, {"type", "source"}, {"source", "builtin"}});
                for (auto& n : reg.ListProcessors())  plugins.push_back({{"name", n}, {"type", "processor"}, {"source", "builtin"}});
                for (auto& n : reg.ListAggregators()) plugins.push_back({{"name", n}, {"type", "aggregator"}, {"source", "builtin"}});
                for (auto& n : reg.ListSinks())       plugins.push_back({{"name", n}, {"type", "sink"}, {"source", "builtin"}});

                // 标记 .so 外部插件（覆盖 builtin 标记）
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

    // ========================================================================
    // POST /api/v1/export — 导出 API
    // ========================================================================
    // 将环形缓冲区中的最近 N 条数据导出为 .ilr 文件。
    // 与录制不同，导出是"回溯"操作——导出已完成采集的数据，不开启新录制。
    //
    // 请求体：
    //   {
    //     "features": ["cpu_utilization", "cpu_profiler"],  // 可选，不指定则导出所有活跃 Feature
    //     "lookback_batches": 60                             // 可选，回溯的批次数（默认 60）
    //   }
    //
    // 响应：
    //   {
    //     "status": "ok",
    //     "file": "/tmp/illuminator_exports/export_1234567890.ilr",
    //     "features_exported": 2,
    //     "batches_exported": 120
    //   }
    srv.Post("/api/v1/export",
             [&features](const httplib::Request& req, httplib::Response& res) {
                 json body;
                 try { body = json::parse(req.body); }
                 catch (...) { JsonError(res, "Invalid JSON body", 400); return; }

                 // 解析要导出的 Feature 列表
                 std::vector<std::string> target_features;
                 if (body.contains("features") && body["features"].is_array()) {
                     for (auto& f : body["features"])
                         target_features.push_back(f.get<std::string>());
                 } else {
                     // 未指定时导出所有活跃 Feature
                     for (const auto& f : features.ListFeatures())
                         if (f.state == FeatureState::kActive)
                             target_features.push_back(f.name);
                 }

                 size_t lookback = body.value("lookback_batches", 60);

                 // 生成文件名：export_<epoch_ms>.ilr
                 auto now = std::chrono::system_clock::now();
                 auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch()).count();
                 std::string filename = "export_" + std::to_string(epoch_ms) + ".ilr";
                 std::string output_dir = "/tmp/illuminator_exports";
                 (void)std::system(("mkdir -p " + output_dir).c_str());
                 std::string path = output_dir + "/" + filename;

                 // 写入文件
                 std::ofstream out(path, std::ios::binary);
                 if (!out.is_open()) {
                     JsonError(res, "Cannot create export file: " + path, 500);
                     return;
                 }

                 // 写入文件头（JSON 格式的元数据）
                 json header;
                 header["format"] = "ilr";
                 header["version"] = 1;
                 header["type"] = "export";
                 header["features"] = target_features;
                 header["started_at"] = epoch_ms;
                 header["lookback_batches"] = lookback;
                 out << header.dump() << "\n";

                 // 写入每个 Feature 的数据
                 size_t total_batches = 0;
                 auto& store = StreamSinkStore::Instance();
                 for (const auto& fname : target_features) {
                     auto& buf = store.GetBuffer(fname);
                     auto recent = buf.Recent(lookback);  // 从环形缓冲区取最近 N 条
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

    // ========================================================================
    // Session API（Tier 3 profiling 会话管理）
    // ========================================================================
    // Session 是 Tier 3 Profiling Feature 的会话管理机制。
    // 用户创建 Session 时指定目标进程和采集时长，Session 到期后自动停止。

    // POST /api/v1/sessions — 创建 profiling 会话
    // 启动一个 Tier 3 Feature 并设置自动过期时间。
    //
    // 请求体：
    //   {
    //     "type": "cpu_profile",           // Feature 类型（如 cpu_profile, offcpu_profile）
    //     "target_pids": [1234, 5678],    // 目标进程 PID（Tier 3 必须）
    //     "duration_sec": 30               // 采集时长（秒），到期后自动停止
    //   }
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

                 // 启动 Feature（Tier 3 安全检查在 FeatureManager::Start 中执行）
                 auto status = features.Start(type, params);
                 if (!status.ok()) {
                     JsonError(res, "Failed to start session: " + status.message(), 400);
                     return;
                 }

                 // 生成会话 ID
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

    // POST /api/v1/sessions/stop — 停止 profiling 会话
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

    // GET /api/v1/sessions — 列出活跃 profiling 会话
    // 返回所有 Tier 3 且处于 Active 状态的 Feature。
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