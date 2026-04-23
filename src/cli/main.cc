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
#include "core/engine/pipeline_controller.h"
#include "plugin/builtin/builtin_plugins.h"
#include "plugin/manager/plugin_manager.h"
#include "server/http_server.h"
#include "sinks/prometheus_exposition/prometheus_sink.h"
#include "storage/storage_backend.h"
#include "core/common/self_observability.h"

static std::atomic<bool> g_running{true};

static void SignalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

static void PrintUsage(const char* prog) {
    fprintf(stderr,
        "Illuminator - Observability & Performance Analysis Platform v0.1.0\n\n"
        "Usage: %s <command> [options]\n\n"
        "Commands:\n"
        "  daemon     Start the Illuminator daemon\n"
        "  collect    One-shot data collection\n"
        "  export     Export stored data to a format\n"
        "  top        Real-time system overview\n"
        "  version    Show version\n"
        "  plugins    List registered plugins\n"
        "  storage    List storage backends\n\n"
        "Options:\n"
        "  --config <path>       Configuration file path\n"
        "  --log-level <level>   Log level (trace/debug/info/warn/error)\n"
        "  --duration <sec>      Collection duration (collect command)\n"
        "  --output <path>       Output file path (export command)\n"
        "  --format <fmt>        Export format (pprof/json/csv/folded)\n\n", prog);
}

static void SetLogLevel(const std::string& level) {
    using illuminator::LogLevel;
    if (level == "trace") illuminator::Logger::Instance().SetLevel(LogLevel::kTrace);
    else if (level == "debug") illuminator::Logger::Instance().SetLevel(LogLevel::kDebug);
    else if (level == "info")  illuminator::Logger::Instance().SetLevel(LogLevel::kInfo);
    else if (level == "warn")  illuminator::Logger::Instance().SetLevel(LogLevel::kWarn);
    else if (level == "error") illuminator::Logger::Instance().SetLevel(LogLevel::kError);
}

static illuminator::GlobalConfig BuildDemoConfig() {
    illuminator::GlobalConfig config;
    config.log_level = "info";

    // System metrics pipeline -> console + storage
    illuminator::PipelineConfig pc;
    pc.name = "system_metrics";
    pc.source.type = "proc_stat_reader";
    pc.source.config["interval_ms"] = int64_t{2000};
    pc.sinks.push_back({"console_output", illuminator::ConfigValue()});

    illuminator::ConfigValue storage_cfg;
    storage_cfg["backend"] = "sqlite";
    storage_cfg["path"] = "/tmp/illuminator_data";
    storage_cfg["pipeline"] = "system_metrics";
    pc.sinks.push_back({"local_storage", storage_cfg});

    config.pipelines.push_back(std::move(pc));
    return config;
}

static int RunDaemon(const std::string& config_path, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Illuminator v0.1.0 starting...");

    illuminator::RegisterBuiltinPlugins();
    illuminator::PluginManager::Instance().PrintRegisteredPlugins();

    auto config = BuildDemoConfig();

    illuminator::PipelineController controller;
    auto status = controller.BuildFromConfig(config);
    if (!status.ok()) {
        IL_ERROR("Failed to build pipelines: %s", status.message().c_str());
        return 1;
    }

    status = controller.StartAll();
    if (!status.ok()) {
        IL_ERROR("Failed to start pipelines: %s", status.message().c_str());
        return 1;
    }

    // Start HTTP server
    illuminator::HttpServer http_server;
    http_server.RegisterHandler("/healthz", [](const std::string&) {
        return "{\"status\":\"ok\",\"version\":\"0.1.0\"}\n";
    });
    http_server.RegisterHandler("/api/v1/pipelines",
        [&controller](const std::string&) {
        std::string result = "{\"pipelines\":[";
        bool first = true;
        for (auto& p : controller.Pipelines()) {
            if (!first) result += ",";
            result += "{\"name\":\"" + p->name() + "\""
                   + ",\"running\":" + (p->IsRunning() ? "true" : "false")
                   + ",\"batches\":" + std::to_string(p->BatchesProcessed())
                   + ",\"records\":" + std::to_string(p->RecordsProcessed())
                   + ",\"errors\":" + std::to_string(p->ErrorCount()) + "}";
            first = false;
        }
        result += "]}\n";
        return result;
    });
    // Self-observability endpoints
    http_server.RegisterHandler("/metrics", [](const std::string&) {
        return illuminator::InternalMetrics::Instance().ExportPrometheus();
    });
    http_server.RegisterHandler("/api/v1/internal_metrics", [](const std::string&) {
        return illuminator::InternalMetrics::Instance().ExportJson() + "\n";
    });

    // Serve static web frontend
    http_server.SetStaticDir("web/dist");
    http_server.Start("0.0.0.0", 9527);

    IL_INFO("Illuminator daemon running. HTTP on :9527. Ctrl+C to stop.");

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    IL_INFO("Shutting down...");
    http_server.Stop();
    controller.StopAll();
    IL_INFO("Illuminator stopped.");
    return 0;
}

static int RunCollect(int duration_sec, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Collecting for %d seconds...", duration_sec);

    illuminator::RegisterBuiltinPlugins();

    auto config = BuildDemoConfig();
    illuminator::PipelineController controller;
    controller.BuildFromConfig(config);
    controller.StartAll();

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
    cfg["interval_ms"] = int64_t{1000};
    source->Init(cfg);

    while (g_running.load()) {
        auto result = source->Collect();
        if (!result.ok()) continue;

        auto& batch = result.value();
        fprintf(stdout, "\033[2J\033[H");  // Clear screen
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

    std::cout << "=== Registered Plugins ===\n\n"
              << "Sources:\n";
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
    std::string config_path, log_level = "info", output, format;
    int duration = 10;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc)
            log_level = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc)
            format = argv[++i];
    }

    if (command == "daemon") return RunDaemon(config_path, log_level);
    if (command == "collect") return RunCollect(duration, log_level);
    if (command == "top") return RunTop(log_level);
    if (command == "version") { std::cout << "Illuminator v0.1.0\n"; return 0; }
    if (command == "plugins") return RunPluginList();
    if (command == "storage") return RunStorageList();

    PrintUsage(argv[0]);
    return 1;
}
