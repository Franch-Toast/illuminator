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
#include "core/common/yaml_config_loader.h"
#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"
#include "plugin/infra/feature_driver_factories.h"
#include "plugin/infra/feature_registry.h"
#include "plugin/features/cpu/cpu_utilization/cpu_utilization_driver.h"
#include "plugin/features/cpu/process_cpu/process_cpu_driver.h"
#include "plugin/features/cpu/cpu_profiler/cpu_profiler_driver.h"
#include "plugin/features/io/io_monitor/io_monitor_driver.h"
#include "plugin/features/net/net_tracer/net_tracer_driver.h"
#include "plugin/features/sched/sched_analyzer/sched_analyzer_driver.h"
#include "plugin/features/sched/offcpu_profiler/offcpu_profiler_driver.h"
#include "plugin/infra/builtin_plugins.h"
#include "plugin/infra/plugin_manager.h"
#include "server/http_server.h"
#include "server/api_routes.h"
#include "server/api_v2_routes.h"
#include "server/sse_handler.h"
#include "core/common/json_serializer.h"
#include "server/storage/storage_backend.h"

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
        auto status = illuminator::InfrastructureManager::Instance().Start(config.engine);
        if (!status.ok()) {
            IL_ERROR("Failed to start InfrastructureManager: {}", status.message());
            return 1;
        }
    }

    illuminator::FeatureDriver::SetDefaultDataDir(config.data_dir);

    // Register all FeatureDrivers and probe when auto_start is enabled
    illuminator::FeatureDriver::SetSsePublishCallback(
        [](const std::string& feature, const std::string& data) {
            illuminator::SseHandler::Instance().Publish(feature, data);
        });
    illuminator::FeatureDriver::SetSseSinkFactory(
        [](const char* name) { return illuminator::MakeFeatureSseSink(name); });
    illuminator::FeatureDriver::SetRecordingSinkFactory(
        [](const std::string& feature, const std::string& dir) {
            return illuminator::MakeFeatureRecordingSinkPair(feature, dir);
        });
    illuminator::FeatureRegistry::RegisterAll();
    if (config.auto_start) {
        illuminator::FeatureBus::Instance().ProbeAll();
        IL_INFO("FeatureBus: all registered drivers probed (auto_start=true)");
    } else {
        IL_INFO("FeatureBus: drivers registered but not probed (auto_start=false, awaiting frontend control)");
    }

    if (!config.server.http_enabled) {
        IL_WARN("HTTP server disabled by config. No REST API or frontend will be served.");
    }

    illuminator::HttpServer http_server;
    illuminator::SetupAuthMiddleware(http_server.server(), config.server.auth_token);
    illuminator::RegisterApiRoutes(http_server.server());

    // SSE data plane
    illuminator::SseHandler::Instance().RegisterRoutes(http_server.server());

    illuminator::RegisterApiV2Routes(http_server.server());

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
    auto& infra = illuminator::InfrastructureManager::Instance();
    infra.GetTimerWheel().Stop();
    illuminator::FeatureBus::Instance().RemoveAll();
    infra.Stop();
    illuminator::SseHandler::Instance().ShutdownAll();
    if (config.server.http_enabled) http_server.Stop();
    IL_INFO("Illuminator stopped.");
    return 0;
}

static int RunCollect(int duration_sec, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Collecting for {} seconds...", duration_sec);

    illuminator::RegisterBuiltinPlugins();

    auto status = illuminator::InfrastructureManager::Instance().Start(
        illuminator::EngineConfig{.collect_pool_threads = 2, .sink_pool_threads = 4});
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
