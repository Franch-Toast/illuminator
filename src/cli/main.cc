// ============================================================================
// Illuminator CLI — 命令行入口
// ============================================================================

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "core/common/logging.h"
#include "core/common/config.h"
#include "core/common/version_generated.h"
#include "core/config/yaml_config_loader.h"
#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"
#include "features/feature_registry.h"
#include "features/cpu_utilization_driver.h"
#include "features/process_cpu_driver.h"
#include "features/cpu_profiler_driver.h"
#include "features/io_monitor_driver.h"
#include "features/net_tracer_driver.h"
#include "features/sched_analyzer_driver.h"
#include "features/offcpu_profiler_driver.h"
#include "plugin/builtin/builtin_plugins.h"
#include "plugin/manager/plugin_manager.h"
#include "server/http_server.h"
#include "server/api_routes.h"
#include "server/sse_handler.h"
#include "serialization/json_serializer.h"
#include "storage/storage_backend.h"

static std::atomic<bool> g_running{true};

static void SignalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

static void PrintUsage(const char* prog) {
    fprintf(stderr,
        "Illuminator - Observability & Performance Analysis Platform v%s\n\n"
        "Usage: %s <command> [options]\n\n"
        "Commands:\n"
        "  daemon     Start the Illuminator daemon\n"
        "  collect    One-shot data collection\n"
        "  top        Real-time system overview\n"
        "  version    Show version\n"
        "  plugins    List registered plugins\n"
        "  storage    List storage backends\n\n"
        "Options:\n"
        "  --config <path>       Configuration file path\n"
        "  --log-level <level>   Log level (trace/debug/info/warn/error)\n"
        "  --duration <sec>      Collection duration (collect command)\n"
        "  --port <port>         HTTP port override\n\n",
        illuminator::kIlluminatorVersion, prog);
}

static void SetLogLevel(const std::string& level) {
    using illuminator::LogLevel;
    if (level == "trace") illuminator::SetLogLevel(LogLevel::kTrace);
    else if (level == "debug") illuminator::SetLogLevel(LogLevel::kDebug);
    else if (level == "info")  illuminator::SetLogLevel(LogLevel::kInfo);
    else if (level == "warn")  illuminator::SetLogLevel(LogLevel::kWarn);
    else if (level == "error") illuminator::SetLogLevel(LogLevel::kError);
}

static constexpr const char* kDefaultConfigYaml = R"yaml(
global:
  log_level: info
  auto_start: false
pipelines:
  cpu_utilization:
    source:
      type: cpu_utilization
      config:
        interval_ms: 1000
        collect_per_core: true
        collect_frequency: false
        ema_alpha: 0.3
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: cpu_utilization
  cpu_processes:
    source:
      type: process_cpu
      config:
        interval_ms: 2000
        top_n: 50
        thread_detail_threshold_pct: 3.0
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: cpu_processes
  cpu_profile:
    source:
      type: cpu_profiler
      config:
        frequency_hz: 49
        mode: aggregated
        user_stacks: true
        kernel_stacks: true
    processors:
      - type: stack_symbolizer
        config:
          demangle: true
          kernel_symbols: true
      - type: stack_merger
        config:
          group_by: comm
          include_kernel: true
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: cpu_profile
      - type: pprof_export
  offcpu_profile:
    source:
      type: offcpu_profiler
      config:
        min_block_us: 1000
        target_tgid: 0
    processors:
      - type: stack_symbolizer
        config:
          demangle: true
          kernel_symbols: true
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: offcpu_profile
  sched_analysis:
    source:
      type: sched_analyzer
      config:
        detailed_mode: false
        aggregate_interval_ms: 5000
        track_migrations: true
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: sched_analysis
)yaml";

static illuminator::GlobalConfig BuildDemoConfig() {
    auto result = illuminator::YamlConfigLoader::LoadFromString(kDefaultConfigYaml);
    if (!result.ok()) {
        IL_FATAL("Failed to parse built-in config: {}", result.status().message());
    }
    return result.value();
}

// Parse "host:port" into (host, port); returns false on failure.
static bool ParseListenAddr(const std::string& listen,
                            std::string& host, int& port) {
    auto colon = listen.rfind(':');
    if (colon == std::string::npos) return false;
    host = listen.substr(0, colon);
    try { port = std::stoi(listen.substr(colon + 1)); }
    catch (...) { return false; }
    return port > 0 && port < 65536;
}

static int RunDaemon(const std::string& config_path, const std::string& log_level,
                     int port_override) {
    SetLogLevel(log_level);
    IL_INFO("Illuminator v{} starting...", illuminator::kIlluminatorVersion);

    illuminator::RegisterBuiltinPlugins();

    illuminator::GlobalConfig config;
    if (!config_path.empty()) {
        IL_INFO("Loading configuration from: {}", config_path);
        auto result = illuminator::YamlConfigLoader::LoadFromFile(config_path);
        if (!result.ok()) {
            IL_ERROR("Failed to load config: {}", result.status().message());
            return 1;
        }
        config = result.value();
        if (!config.log_level.empty()) SetLogLevel(config.log_level);
        if (!config.log_file.empty())
            illuminator::ConfigureFileLogging(
                config.log_file, config.log_max_size, config.log_max_files);
    } else {
        IL_INFO("No config file specified, using built-in demo configuration");
        config = BuildDemoConfig();
    }

    if (!config.plugin_dirs.empty()) {
        auto pm_status = illuminator::PluginManager::Instance()
            .LoadPluginsFromDirs(config.plugin_dirs);
        if (!pm_status.ok()) {
            IL_WARN("Plugin loading issue: {}", pm_status.message());
        }
    }
    illuminator::PluginManager::Instance().PrintRegisteredPlugins();

    std::string http_host = "127.0.0.1";
    int http_port = 9527;
    ParseListenAddr(config.server.http_listen, http_host, http_port);
    if (port_override > 0) http_port = port_override;

    // ---- RFC v3: FeatureBus is the ONLY pipeline management path ----
    // Start InfrastructureManager (TimerWheel + CollectPool + SinkPool)
    {
        illuminator::InfrastructureConfig infra_cfg;
        infra_cfg.collect_pool_threads = config.engine.collect_pool_threads > 0
            ? config.engine.collect_pool_threads : 2;
        infra_cfg.sink_pool_threads = config.engine.sink_pool_threads;
        auto status = illuminator::InfrastructureManager::Instance().Start(infra_cfg);
        if (!status.ok()) {
            IL_ERROR("Failed to start InfrastructureManager: {}", status.message());
            return 1;
        }
    }

    // Register all FeatureDrivers and probe Tier 1/2 (always-on)
    illuminator::FeatureRegistry::RegisterAll();
    illuminator::FeatureBus::Instance().ProbeAll();
    IL_INFO("FeatureBus: all registered drivers probed");

    if (!config.server.http_enabled) {
        IL_WARN("HTTP server disabled by config. No REST API or frontend will be served.");
    }

    illuminator::HttpServer http_server;
    illuminator::SetupAuthMiddleware(http_server.server(), config.server.auth_token);
    illuminator::RegisterApiRoutes(http_server.server());

    // SSE data plane
    illuminator::SseHandler::Instance().RegisterRoutes(http_server.server());

    // ---- FeatureBus REST API routes (/api/v2/features/*) ----
    {
        auto& svr = http_server.server();
        auto& bus = illuminator::FeatureBus::Instance();

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
                auto* drv = bus.GetDriver(d.name);
                if (drv) {
                    j["state"] = illuminator::DriverStateToString(drv->State());
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
                    res.status = (status.code() == illuminator::StatusCode::kNotFound) ? 404 : 400;
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
                        illuminator::ConfigValue cfg;
                        for (auto& [key, val] : body.items()) {
                            if (val.is_array()) {
                                // Convert JSON array to comma-separated string
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
                        auto reconf_status = bus.Reconfigure(name, cfg);
                        if (!reconf_status.ok()) {
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

        svr.Get(R"(/api/v2/features/([a-zA-Z0-9_-]+)/stats)",
            [&bus](const httplib::Request& req, httplib::Response& res) {
                auto name = req.matches[1].str();
                auto* drv = bus.GetDriver(name);
                if (!drv) {
                    res.status = 404;
                    res.set_content(R"({"error":"feature not found"})", "application/json");
                    return;
                }
                auto stats = drv->GetStats();
                nlohmann::json j = {
                    {"batches_processed", stats.batches_processed},
                    {"records_processed", stats.records_processed},
                    {"errors", stats.errors},
                    {"uptime_ms", stats.uptime_ms}
                };
                res.set_content(j.dump(), "application/json");
            });

        IL_INFO("FeatureBus REST routes registered under /api/v2/features/*");
    }

    http_server.SetStaticDir("web/dist");
    if (config.server.http_enabled) {
        http_server.Start(http_host, http_port);
    }

    IL_INFO("Illuminator daemon running on {}:{} (HTTP + SSE). Ctrl+C to stop.",
            http_host, http_port);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    IL_INFO("Shutting down...");
    illuminator::FeatureBus::Instance().RemoveAll();
    illuminator::InfrastructureManager::Instance().Stop();
    if (config.server.http_enabled) http_server.Stop();
    IL_INFO("Illuminator stopped.");
    return 0;
}

static int RunCollect(int duration_sec, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Collecting for {} seconds...", duration_sec);

    illuminator::RegisterBuiltinPlugins();

    illuminator::InfrastructureConfig infra_cfg;
    infra_cfg.collect_pool_threads = 2;
    infra_cfg.sink_pool_threads = 4;
    auto status = illuminator::InfrastructureManager::Instance().Start(infra_cfg);
    if (!status.ok()) {
        IL_ERROR("Failed to start InfrastructureManager: {}", status.message());
        return 1;
    }

    illuminator::FeatureRegistry::RegisterAll();
    illuminator::FeatureBus::Instance().ProbeAll();

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

    illuminator::FeatureBus::Instance().RemoveAll();
    illuminator::InfrastructureManager::Instance().Stop();
    IL_INFO("Collection complete.");
    return 0;
}

static int RunTop(const std::string& log_level) {
    SetLogLevel(log_level);
    illuminator::RegisterBuiltinPlugins();

    fprintf(stdout, "Illuminator Top - Press Ctrl+C to exit\n\n");
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    auto& registry = illuminator::PluginRegistry::Instance();
    auto source = registry.CreateSource("proc_stat_reader");
    if (!source) {
        IL_ERROR("proc_stat_reader not available");
        return 1;
    }

    illuminator::ConfigValue cfg;
    cfg.Set("interval_ms", int64_t{1000});
    source->Init(cfg);

    while (g_running.load()) {
        auto result = source->Collect();
        if (!result.ok()) continue;

        auto& batch = result.value();
        fprintf(stdout, "\033[2J\033[H");
        fprintf(stdout, "Illuminator Top - %s\n",
                batch->GetMeta("_time", "").c_str());
        fprintf(stdout, "%-10s %12s %12s %12s %12s\n",
                "CPU", "User%", "System%", "Idle%", "IOWait%");
        fprintf(stdout, "--------------------------------------------------------------\n");

        for (auto& rec : batch->records()) {
            bool is_cpu = false;
            std::string cpu_name;
            for (auto& l : rec.labels) {
                if (l.key == "cpu") {
                    is_cpu = true;
                    cpu_name = std::string(l.value);
                }
            }
            if (!is_cpu) continue;

            auto get = [&rec](const char* name) -> uint64_t {
                auto it = rec.fields.find(name);
                if (it == rec.fields.end()) return 0;
                if (auto* v = std::get_if<uint64_t>(&it->second)) return *v;
                return 0;
            };

            uint64_t user = get("user"), system = get("system");
            uint64_t idle = get("idle"), iowait = get("iowait");
            uint64_t total = user + system + idle + iowait + get("nice") +
                           get("irq") + get("softirq") + get("steal");
            if (total == 0) total = 1;

            fprintf(stdout, "%-10s %11.1f%% %11.1f%% %11.1f%% %11.1f%%\n",
                    cpu_name.c_str(),
                    100.0 * user / total,
                    100.0 * system / total,
                    100.0 * idle / total,
                    100.0 * iowait / total);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

static int RunPluginList() {
    illuminator::RegisterBuiltinPlugins();
    auto& reg = illuminator::PluginRegistry::Instance();

    std::cout << "=== Registered Plugins ===\n\n";
    std::cout << "Sources:\n";
    for (auto& n : reg.ListSources()) std::cout << "  - " << n << "\n";
    std::cout << "\nProcessors:\n";
    for (auto& n : reg.ListProcessors()) std::cout << "  - " << n << "\n";
    std::cout << "\nAggregators:\n";
    for (auto& n : reg.ListAggregators()) std::cout << "  - " << n << "\n";
    std::cout << "\nSinks:\n";
    for (auto& n : reg.ListSinks()) std::cout << "  - " << n << "\n";
    return 0;
}

static int RunStorageList() {
    illuminator::RegisterBuiltinPlugins();
    auto names = illuminator::StorageFactory::Instance().Available();
    std::cout << "=== Available Storage Backends ===\n";
    for (auto& n : names) std::cout << "  - " << n << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    std::string config_path, log_level = "info";
    int duration = 10;
    int port_override = 0;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            log_level = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port_override = std::atoi(argv[++i]);
    }

    if (command == "daemon") return RunDaemon(config_path, log_level, port_override);
    if (command == "collect") return RunCollect(duration, log_level);
    if (command == "top") return RunTop(log_level);
    if (command == "version") {
        std::cout << "Illuminator v" << illuminator::kBuildVersion << "\n"
                  << "  commit: " << illuminator::kBuildCommit << "\n";
        return 0;
    }
    if (command == "plugins") return RunPluginList();
    if (command == "storage") return RunStorageList();

    PrintUsage(argv[0]);
    return 1;
}
