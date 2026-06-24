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
#include "core/engine/feature_manager.h"
#include "core/engine/pipeline_controller.h"
#include "plugin/builtin/builtin_plugins.h"
#include "plugin/manager/plugin_manager.h"
#include "server/http_server.h"
#include "server/api_routes.h"
#include "server/websocket_manager.h"
#include "storage/storage_backend.h"
#include "sinks/local_storage/local_storage_sink.h"
#include "serialization/json_serializer.h"

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
        "  --port <port>         HTTP port override\n"
        "  --ws-port <port>      WebSocket port override\n\n",
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
                     int port_override, int ws_port_override) {
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

    // 加载 .so 外部插件（如果配置了 plugin_dirs）
    if (!config.plugin_dirs.empty()) {
        auto pm_status = illuminator::PluginManager::Instance()
            .LoadPluginsFromDirs(config.plugin_dirs);
        if (!pm_status.ok()) {
            IL_WARN("Plugin loading issue: {}", pm_status.message());
        }
    }
    illuminator::PluginManager::Instance().PrintRegisteredPlugins();

    // Parse HTTP address from config (default "127.0.0.1:9527")
    // WebSocket now shares the same port via HTTP Upgrade mechanism.
    std::string http_host = "127.0.0.1";
    int http_port = 9527;
    ParseListenAddr(config.server.http_listen, http_host, http_port);

    if (port_override > 0) http_port = port_override;
    (void)ws_port_override; // deprecated: WS now uses same port as HTTP

    illuminator::PipelineController controller;
    auto status = controller.BuildFromConfig(config);
    if (!status.ok()) {
        IL_ERROR("Failed to build pipelines: {}", status.message());
        return 1;
    }

    // 按需启动模式：pipeline 由 FeatureManager API 控制启停
    // 仅当配置 auto_start=true 时才全量自启（兼容旧行为）
    if (config.auto_start) {
        status = controller.StartAll();
        if (!status.ok()) {
            IL_ERROR("Failed to start pipelines: {}", status.message());
            return 1;
        }
        IL_INFO("Auto-start: all pipelines running");
    } else {
        controller.GetTimerWheel().Start();
        IL_INFO("On-demand mode: pipelines await Feature API start commands");
    }

    // Expose the first storage backend for the query API
    for (auto& p : controller.Pipelines()) {
        for (auto& sink : p->GetSinks()) {
            if (auto* ls = dynamic_cast<illuminator::LocalStorageSink*>(sink.get())) {
                if (ls->GetBackend()) {
                    controller.SetStorageBackend(ls->GetBackend());
                    IL_INFO("Query API using storage from pipeline '{}'", p->name());
                    goto storage_found;
                }
            }
        }
    }
    storage_found:

    illuminator::WebSocketManager ws_manager;
    ws_manager.SetBroadcastInterval(1000);
    ws_manager.SetAuthToken(config.server.auth_token);
    ws_manager.SetSerializer([](const std::string& pipeline_key,
                                illuminator::DataBatchPtr batch) -> std::string {
        // Profiling features: send lightweight notification (data is large)
        // Frontend will pull incremental data via featureStream API
        if (pipeline_key.find("profile") != std::string::npos) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return "{\"type\":\"notify\",\"feature\":\"" + pipeline_key +
                   "\",\"records\":" + std::to_string(batch->records().size()) +
                   ",\"ts\":" + std::to_string(now_ms) + "}";
        }
        // Tier 1-2 monitoring: send full batch via WS
        return illuminator::BatchToJson(*batch, pipeline_key);
    });

    // FeatureManager: register all configured pipelines as features
    illuminator::FeatureManager feature_manager(controller);

    auto resolve_category = [](const std::string& name) -> std::string {
        if (name.find("cpu") != std::string::npos ||
            name.find("sched") != std::string::npos ||
            name.find("offcpu") != std::string::npos) return "cpu";
        if (name.find("mem") != std::string::npos ||
            name.find("heap") != std::string::npos) return "memory";
        if (name.find("io") != std::string::npos ||
            name.find("disk") != std::string::npos) return "io";
        if (name.find("net") != std::string::npos ||
            name.find("tcp") != std::string::npos) return "network";
        if (name.find("gpu") != std::string::npos) return "gpu";
        return "system";
    };

    auto resolve_display_name = [](const std::string& name) -> std::string {
        if (name == "cpu_utilization") return "CPU Utilization";
        if (name == "cpu_processes") return "Process CPU (Top-N)";
        if (name == "cpu_profile") return "CPU Profile (On-CPU)";
        if (name == "offcpu_profile") return "Off-CPU Analysis";
        if (name == "sched_analysis") return "Scheduler Analysis";
        return name;
    };

    auto resolve_tier = [](const std::string& name) -> illuminator::FeatureTier {
        if (name == "cpu_utilization" || name == "cpu_processes" ||
            name == "memory_utilization" || name == "memory_processes" ||
            name == "gpu_monitor")
            return illuminator::FeatureTier::kMonitoring;
        if (name == "sched_analysis" || name == "io_monitor" ||
            name == "net_tracer")
            return illuminator::FeatureTier::kTracing;
        if (name == "cpu_profile" || name == "offcpu_profile" ||
            name == "heap_profiler")
            return illuminator::FeatureTier::kProfiling;
        return illuminator::FeatureTier::kMonitoring;
    };

    for (const auto& pc : config.pipelines) {
        illuminator::FeatureConfig fc;
        fc.name = pc.name;
        fc.display_name = resolve_display_name(pc.name);
        fc.category = resolve_category(pc.name);
        fc.tier = resolve_tier(pc.name);
        fc.pipeline = pc;
        feature_manager.RegisterFeature(std::move(fc));
    }
    IL_INFO("FeatureManager: {} features registered", config.pipelines.size());

    // Always-On: auto-start Tier 1-2 features (monitoring + tracing)
    {
        int auto_started = 0;
        for (const auto& f : feature_manager.ListFeatures()) {
            if (static_cast<int>(f.tier) <= 2 && f.state == illuminator::FeatureState::kInactive) {
                illuminator::FeatureManager::StartParams auto_params;
                auto status = feature_manager.Start(f.name, auto_params);
                if (status.ok()) {
                    ++auto_started;
                } else {
                    IL_WARN("Auto-start failed for '{}': {}", f.name, status.message());
                }
            }
        }
        IL_INFO("Always-On: {}/{} features auto-started (Tier 1-2)",
                auto_started, config.pipelines.size());
    }

    illuminator::HttpServer http_server;
    illuminator::SetupAuthMiddleware(http_server.server(), config.server.auth_token);
    illuminator::RegisterApiRoutes(http_server.server(), controller, &feature_manager);
    illuminator::RegisterFeatureRoutes(http_server.server(), feature_manager);

    // Same-port WebSocket: upgrade handler intercepts WS requests on HTTP port
    http_server.SetWebSocketUpgradeHandler(
        [&ws_manager](int fd, const std::string& raw_request) -> bool {
            return ws_manager.HandleUpgrade(fd, raw_request);
        });

    http_server.SetStaticDir("web/dist");
    http_server.Start(http_host, http_port);
    ws_manager.Start();

    IL_INFO("Illuminator daemon running on {}:{} (HTTP + WS same port). Ctrl+C to stop.",
            http_host, http_port);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    IL_INFO("Shutting down...");
    feature_manager.StopAll();
    ws_manager.Stop();
    http_server.Stop();
    controller.StopAll();
    IL_INFO("Illuminator stopped.");
    return 0;
}

static int RunCollect(int duration_sec, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Collecting for {} seconds...", duration_sec);

    illuminator::RegisterBuiltinPlugins();
    auto config = BuildDemoConfig();
    illuminator::PipelineController controller;

    auto build_st = controller.BuildFromConfig(config);
    if (!build_st.ok()) {
        IL_ERROR("Failed to build pipelines: {}", build_st.message());
        return 1;
    }

    auto start_st = controller.StartAll();
    if (!start_st.ok()) {
        IL_ERROR("Failed to start pipelines: {}", start_st.message());
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

    controller.StopAll();
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
    int port_override = 0, ws_port_override = 0;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            log_level = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port_override = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--ws-port") == 0 && i + 1 < argc)
            ws_port_override = std::atoi(argv[++i]);
    }

    if (command == "daemon") return RunDaemon(config_path, log_level, port_override, ws_port_override);
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
