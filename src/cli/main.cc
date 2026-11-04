// Minimal host for the core SDK. Pipeline assembly and scheduling live here.
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/version_generated.h"
#include "core/common/yaml_config_loader.h"
#include "core/engine/infrastructure_manager.h"
#include "core/engine/pipeline.h"
#include "plugin/infra/builtin_plugins.h"
#include "plugin/infra/plugin_manager.h"
#include "plugin/sinks/recording_sink/recording_sink.h"
#include "storage/sqlite_backend/sqlite_backend.h"

namespace {
using namespace illuminator;
volatile std::sig_atomic_t interrupted = 0;
void SignalHandler(int) { interrupted = 1; }

// Synthetic example only: it observes no system resource and needs no privileges.
class DemoSource : public SourcePlugin {
public:
    const char* Name() const override { return "demo_counter"; }
    const char* Version() const override { return "0.1.0"; }
    uint32_t IntervalMs() const override { return 50; }
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& record = batch->AddRecord();
        record.timestamp = NowTimestamp();
        record.SetField(batch->InternString("value"), ++count_);
        return batch;
    }
private:
    int64_t count_ = 0;
};

struct Options {
    std::string command;
    std::string config;
    std::string source;
    std::string sink = "recording";
    std::string output_dir;
    int duration = 1;
};

void Usage() {
    std::cout << "Illuminator core runtime\n"
              << "Usage: illuminator <demo|run|plugins|storage|version> [options]\n"
              << "  demo                 Run a synthetic Source -> RecordingSink pipeline\n"
              << "  run --source <name>  Run a registered source (including .so plugins)\n"
              << "  --sink <name>        Registered output (default: recording)\n"
              << "  --config <path>      Core YAML configuration\n"
              << "  --duration <sec>     Run for 1..86400 seconds (default: 1)\n"
              << "  --output-dir <path>  Recording directory (default: global.data_dir)\n";
}

// Keep all producers alive until scheduling and outstanding collection are stopped.
// Pipeline::Stop drains its processing channel and waits for its sink writes.
struct RuntimeShutdown {
    InfrastructureManager& infrastructure;
    Pipeline& pipeline;
    bool stopped = false;
    void Stop() {
        if (stopped) return;
        infrastructure.GetTimerWheel().Stop();
        infrastructure.GetCollectPool()->DrainAndStop();
        pipeline.Stop();
        infrastructure.Stop();
        stopped = true;
    }
    ~RuntimeShutdown() { Stop(); }
};

int Run(const Options& options, const GlobalConfig& config) {
    auto& registry = PluginRegistry::Instance();
    if (options.command == "demo") {
        registry.RegisterSource("demo_counter", [] { return std::make_unique<DemoSource>(); });
    }
    const std::string source_name = options.command == "demo" ? "demo_counter" : options.source;
    auto source = registry.CreateSource(source_name);
    auto sink = registry.CreateSink(options.sink);
    if (!source || !sink) {
        IL_ERROR("Unknown source '{}' or sink '{}'", source_name, options.sink);
        return 1;
    }
    auto status = source->Init(ConfigValue{});
    if (!status.ok()) { IL_ERROR("Source Init: {}", status.message()); return 1; }
    if (source->IntervalMs() == 0) {
        IL_ERROR("Source collection interval must be positive");
        return 1;
    }
    ConfigValue sink_config;
    sink_config.Set("feature_name", source_name); // .ilr v1 stream name
    sink_config.Set("output_dir", options.output_dir.empty() ? config.data_dir : options.output_dir);
    status = sink->Init(sink_config);
    if (!status.ok()) { IL_ERROR("Sink Init: {}", status.message()); return 1; }

    auto& infrastructure = InfrastructureManager::Instance();
    status = infrastructure.Start(config.engine);
    if (!status.ok()) { IL_ERROR("Infrastructure: {}", status.message()); return 1; }
    const auto& channel = config.engine.channel;
    Pipeline pipeline(source_name, ResolveChannelCapacity(channel.size),
                      ResolveDropPolicy(channel.drop_policy),
                      channel.backpressure_high, channel.backpressure_low);
    RuntimeShutdown shutdown{infrastructure, pipeline};
    auto* recording = dynamic_cast<RecordingSink*>(sink.get());
    pipeline.SetSource(std::move(source));
    pipeline.AddSink(std::move(sink));
    pipeline.SetSinkPool(infrastructure.GetSinkPool());
    status = pipeline.Start();
    if (!status.ok()) { IL_ERROR("Pipeline Start: {}", status.message()); return 1; }
    if (recording) {
        status = recording->StartRecordingWithStatus();
        if (!status.ok()) { IL_ERROR("Recording: {}", status.message()); return 1; }
    }

    auto busy = std::make_shared<std::atomic<bool>>(false);
    auto collect_errors = std::make_shared<std::atomic<uint64_t>>(0);
    auto* collect_pool = infrastructure.GetCollectPool();
    infrastructure.GetTimerWheel().AddRepeating(
        std::chrono::milliseconds(pipeline.GetSource()->IntervalMs()),
        [&pipeline, collect_pool, busy, collect_errors] {
            if (busy->exchange(true)) return; // Never overlap Collect on one source.
            collect_pool->Submit([&pipeline, busy, collect_errors] {
                struct ClearBusy {
                    std::atomic<bool>& busy;
                    ~ClearBusy() { busy.store(false); }
                } clear{*busy};
                try {
                    auto batch = pipeline.GetSource()->Collect();
                    if (batch.ok()) pipeline.Enqueue(std::move(*batch));
                    else {
                        ++*collect_errors;
                        IL_ERROR("Collect: {}", batch.status().message());
                    }
                } catch (const std::exception& error) {
                    ++*collect_errors;
                    IL_ERROR("Collect: {}", error.what());
                }
            });
        });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(options.duration);
    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    shutdown.Stop();
    std::cout << "batches=" << pipeline.BatchesProcessed()
              << " records=" << pipeline.RecordsProcessed()
              << " errors=" << pipeline.ErrorCount() + collect_errors->load() << '\n';
    if (recording) {
        auto session = recording->GetSession();
        std::cout << "recorded_batches=" << session.batches_written
                  << " recording=" << session.file_path << '\n';
    }
    return pipeline.ErrorCount() || collect_errors->load() ||
           (options.command == "demo" && pipeline.BatchesProcessed() == 0) ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace illuminator;
    if (argc < 2) { Usage(); return 2; }
    Options options;
    options.command = argv[1];
    if (options.command == "--help" || options.command == "help") { Usage(); return 0; }
    if (options.command != "demo" && options.command != "run" &&
        options.command != "plugins" && options.command != "storage" &&
        options.command != "version") { Usage(); return 2; }
    for (int i = 2; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--help") { Usage(); return 0; }
        if (i + 1 == argc) { std::cerr << "Missing value: " << option << '\n'; return 2; }
        std::string value = argv[++i];
        if (option == "--config") options.config = value;
        else if (option == "--source") options.source = value;
        else if (option == "--sink") options.sink = value;
        else if (option == "--output-dir") options.output_dir = value;
        else if (option == "--duration") {
            auto result = std::from_chars(value.data(), value.data() + value.size(), options.duration);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                options.duration < 1 || options.duration > 86400) {
                std::cerr << "Invalid duration: " << value << '\n'; return 2;
            }
        } else { std::cerr << "Unknown option: " << option << '\n'; return 2; }
    }
    if (options.command == "version") {
        std::cout << kBuildVersion << '\n'; return 0;
    }
    if (options.command == "run" && options.source.empty()) {
        std::cerr << "run requires --source <name>\n"; return 2;
    }
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    try {
        GlobalConfig config;
        if (!options.config.empty()) {
            auto loaded = YamlConfigLoader::LoadFromFile(options.config);
            if (!loaded.ok()) { std::cerr << loaded.status().message() << '\n'; return 1; }
            config = std::move(*loaded);
        }
        GetDefaultLogger();
        spdlog::set_level(spdlog::level::from_str(config.log_level));
        ConfigureFileLogging(config.log_file, config.log_max_size, config.log_max_files);
        RegisterBuiltinPlugins();
        auto& manager = PluginManager::Instance();
        auto status = manager.LoadPluginsFromDirs(config.plugin_dirs);
        if (!status.ok()) { IL_ERROR("Plugin loading: {}", status.message()); return 1; }
        if (options.command == "plugins") { manager.PrintRegisteredPlugins(); return 0; }
        if (options.command == "storage") {
            for (const auto& name : StorageFactory::Instance().Available()) std::cout << name << '\n';
            return 0;
        }
        return Run(options, config);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
