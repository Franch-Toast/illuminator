// ============================================================================
// Illuminator HTTP API v2 路由注册 — Feature 管理
// ============================================================================
//
// 【架构定位】
// 本文件将 /api/v2/features/* REST API 端点注册到 httplib::Server。
// 拆分自 main.cc，与 api_routes.h（v1 路由）并列，职责单一化。
//
// 【端点列表】
//   GET  /api/v2/features                              Feature 列表
//   GET  /api/v2/features/:name/config/schema          配置 schema
//   GET  /api/v2/features/:name/config                 当前配置
//   POST /api/v2/features/:name/config                 更新配置
//   POST /api/v2/features/:name/start                  启动 Feature
//   POST /api/v2/features/:name/stop                   停止 Feature
//   POST /api/v2/features/:name/pause                  暂停 Feature
//   POST /api/v2/features/:name/resume                 恢复 Feature
//   POST /api/v2/features/:name/reconfigure            运行时动态重配置
//   GET  /api/v2/features/:name/stats                  Feature 统计
// ============================================================================

#pragma once

#include <exception>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"

namespace illuminator {

inline ConfigValue JsonToConfigValue(const nlohmann::json& body) {
    ConfigValue cfg;
    for (auto& [key, val] : body.items()) {
        if (val.is_array()) {
            std::string joined;
            for (size_t i = 0; i < val.size(); ++i) {
                if (i > 0) joined += ",";
                if (val[i].is_number()) {
                    joined += std::to_string(val[i].get<int64_t>());
                } else {
                    joined += val[i].get<std::string>();
                }
            }
            cfg.Set(key, joined);
        } else if (val.is_number_integer()) {
            cfg.Set(key, static_cast<int64_t>(val.get<int64_t>()));
        } else if (val.is_number()) {
            cfg.Set(key, std::to_string(val.get<double>()));
        } else if (val.is_boolean()) {
            cfg.Set(key, val.get<bool>() ? "true" : "false");
        } else if (val.is_string()) {
            cfg.Set(key, val.get<std::string>());
        }
    }
    return cfg;
}

// ============================================================================
// RegisterApiV2Routes — 注册 FeatureBus v2 API 路由
// ============================================================================
inline void RegisterApiV2Routes(httplib::Server& svr) {
    auto& bus = FeatureBus::Instance();

    svr.Get("/api/v2/features", [&bus](const httplib::Request&, httplib::Response& res) {
        auto descriptors = bus.ListDescriptors();
        nlohmann::json arr = nlohmann::json::array();
        for (auto& d : descriptors) {
            nlohmann::json j;
            j["name"] = d.name;
            j["display_name"] = d.display_name;
            j["description"] = d.description;
            j["category"] = d.category;
            j["version"] = d.version;
            j["tier"] = static_cast<int>(d.tier);
            j["model"] = static_cast<int>(d.model);
            j["supports_pull"] = d.supports_pull;
            j["supports_push"] = d.supports_push;
            j["supports_pause"] = d.supports_pause;
            j["supports_configure"] = d.supports_configure;
            j["has_bpf_probe"] = d.has_bpf_probe;
            j["session_required"] = d.session_required;
            auto drv = bus.GetDriver(d.name);
            if (drv) {
                j["state"] = DriverStateToString(drv->State());
                auto stats = drv->GetStats();
                j["batches_processed"] = stats.batches_processed;
                j["records_processed"] = stats.records_processed;
                j["errors"] = stats.errors;
                j["uptime_ms"] = stats.uptime_ms;
            }
            arr.push_back(std::move(j));
        }
        nlohmann::json resp;
        resp["features"] = std::move(arr);
        res.set_content(resp.dump(), "application/json");
    });

    svr.Get(R"(/api/v2/features/([a-zA-Z0-9_-]+)/config/schema)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto schema = bus.GetConfigSchema(name);
            if (schema.empty()) {
                res.status = 404;
                res.set_content(R"({"error":"feature not found"})", "application/json");
                return;
            }
            res.set_content(schema, "application/json");
        });

    svr.Get(R"(/api/v2/features/([a-zA-Z0-9_-]+)/config)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto cfg = bus.GetConfig(name);
            if (cfg.empty()) {
                res.status = 404;
                res.set_content(R"({"error":"feature not found"})", "application/json");
                return;
            }
            res.set_content(cfg, "application/json");
        });

    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/config)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto status = bus.SetConfig(name, req.body);
            if (!status.ok()) {
                res.status = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.set_content(R"({"ok":true})", "application/json");
        });

    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/start)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto status = bus.Probe(name);
            if (!status.ok()) {
                res.status = 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            // Apply optional config params from request body (e.g. target_pids for profiler)
            if (!req.body.empty()) {
                try {
                    auto body = nlohmann::json::parse(req.body);
                    auto cfg = JsonToConfigValue(body);
                    auto reconf_status = bus.Reconfigure(name, cfg);
                    if (!reconf_status.ok() &&
                        reconf_status.code() != StatusCode::kRequiresRestart) {
                        IL_WARN("FeatureBus: reconfigure '{}' after start failed: {}", name, reconf_status.message());
                    }
                } catch (const std::exception& e) {
                    IL_WARN("FeatureBus: failed to parse start config for '{}': {}", name, e.what());
                }
            }
            res.set_content(R"({"ok":true})", "application/json");
        });

    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/stop)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto status = bus.Remove(name);
            if (!status.ok()) {
                res.status = 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.set_content(R"({"ok":true})", "application/json");
        });

    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/pause)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto status = bus.Pause(name);
            if (!status.ok()) {
                res.status = 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.set_content(R"({"ok":true})", "application/json");
        });

    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/resume)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto status = bus.Resume(name);
            if (!status.ok()) {
                res.status = 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.set_content(R"({"ok":true})", "application/json");
        });

    // POST /api/v2/features/:name/reconfigure — 运行时动态重配置
    // 接收 JSON body，解析为 ConfigValue，调用 driver->Reconfigure()。
    // Reconfigure 贯穿 Pipeline 全链路（Source → Processor → Aggregator → Sink）。
    svr.Post(R"(/api/v2/features/([a-zA-Z0-9_-]+)/reconfigure)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            if (req.body.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"empty request body"})", "application/json");
                return;
            }
            ConfigValue cfg;
            try {
                auto body = nlohmann::json::parse(req.body);
                cfg = JsonToConfigValue(body);
            } catch (const std::exception& e) {
                res.status = 400;
                nlohmann::json err = {{"error", std::string("invalid JSON: ") + e.what()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            auto status = bus.Reconfigure(name, cfg);
            if (!status.ok() &&
                status.code() != StatusCode::kRequiresRestart) {
                res.status = (status.code() == StatusCode::kNotFound) ? 404 : 400;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            nlohmann::json resp = {{"ok", true}};
            if (status.code() == StatusCode::kRequiresRestart) {
                resp["requires_restart"] = true;
            }
            res.set_content(resp.dump(), "application/json");
        });

    // GET /api/v2/features/:name/query?q=threads&pid=123
    svr.Get(R"(/api/v2/features/([a-zA-Z0-9_-]+)/query)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto drv = bus.GetDriver(name);
            if (!drv) {
                res.status = 404;
                nlohmann::json err = {{"error", "feature not found: " + name}};
                res.set_content(err.dump(), "application/json");
                return;
            }

            std::string query_name = req.get_param_value("q");
            if (query_name.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"missing 'q' parameter"})", "application/json");
                return;
            }

            QueryParams params;
            for (auto& [k, v] : req.params) {
                if (k != "q") params[k] = v;
            }

            auto result = drv->QueryExtra(query_name, params);
            if (!result.ok()) {
                res.status = (result.status().code() == StatusCode::kUnimplemented) ? 501 : 400;
                nlohmann::json err = {{"error", result.status().message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.set_content(result.value(), "application/json");
        });

    svr.Get(R"(/api/v2/features/([a-zA-Z0-9_-]+)/stats)",
        [&bus](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto drv = bus.GetDriver(name);
            if (!drv) {
                res.status = 404;
                res.set_content(R"({"error":"feature not found"})", "application/json");
                return;
            }
            auto stats = drv->GetStats();
            nlohmann::json j = {
                {"state", DriverStateToString(drv->State())},
                {"batches_processed", stats.batches_processed},
                {"records_processed", stats.records_processed},
                {"errors", stats.errors},
                {"uptime_ms", stats.uptime_ms},
                {"bpf_total_events", stats.bpf_total_events},
                {"bpf_buffer_full", stats.bpf_buffer_full},
                {"bpf_dropped", stats.bpf_dropped},
                {"bpf_filtered", stats.bpf_filtered}
            };
            res.set_content(j.dump(), "application/json");
        });

    IL_INFO("FeatureBus REST routes registered under /api/v2/features/*");
}

}  // namespace illuminator
