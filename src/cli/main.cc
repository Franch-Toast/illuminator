// ============================================================================
// Illuminator CLI 入口 — 命令行工具主入口
// ============================================================================
//
// 本文件是 Illuminator 程序的入口点（main() 函数），提供命令行交互界面。
//
// 支持的命令（共 6 个）：
// =========================
// 1. daemon    启动守进程（HTTP 服务 + 多管道并行运行）
// 2. collect   一次性数据采集（运行指定秒数后停止）
// 3. top       实时系统概览（类 htop 终端界面）
// 4. version   输出版本号
// 5. plugins   列出所有已注册的插件
// 6. storage   列出可用的存储后端
//
// daemon 模式初始化流程：
// ========================
//   1. 设置日志级别
//   2. RegisterBuiltinPlugins() — 强制链接所有内置插件
//   3. 加载配置文件（如有），否则使用 BuildDemoConfig() 生成内建 Demo 配置
//   4. PipelineController::BuildFromConfig() — 创建所有管道
//   5. PipelineController::StartAll() — 启动所有管道
//   6. 启动 HTTP 服务器（注册所有 API 端点 + 静态文件服务）
//   7. 注册信号处理（SIGINT/SIGTERM），进入主循环等待停止信号
//
// BuildDemoConfig() 自动创建 4 条 Pipeline：
// ===========================================
//   1. cpu_utilization — 系统 CPU 利用率（→ local_storage Sink）
//   2. cpu_processes   — 进程 CPU 监控（→ local_storage Sink）
//   3. cpu_profile     — CPU 性能剖析（→ stack_symbolizer → stack_merger →
//                         → local_storage + pprof_export 双 Sink）
//   4. sched_analysis  — 调度分析（→ local_storage Sink）
//
// HTTP API 端点（共 8 个）：
// ==========================
//   /healthz                     — 健康检查
//   /api/v1/pipelines            — 管道状态列表
//   /api/v1/cpu/utilization      — CPU 利用率快照
//   /api/v1/cpu/processes        — 进程 CPU 快照
//   /api/v1/cpu/profile/flamegraph — 火焰图数据（含符号化 + 堆栈合并）
//   /api/v1/cpu/sched/summary    — 调度器分析摘要
//   /metrics                     — Prometheus 格式内部指标
//   /api/v1/internal_metrics     — JSON 格式内部指标
//
// 辅助工具函数（在 illuminator 命名空间中）：
// ===========================================
//   EscapeJson()       — JSON 字符串转义
//   FieldValueToJson() — FieldValue 变体转 JSON 字符串
//   StackFramesToJson()— 堆栈帧列表转 JSON 数组
//   BatchToJson()      — 完整 DataBatch 转 JSON（labels + fields + stack_samples）
// ============================================================================

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

#include "core/common/logging.h"
#include "core/common/config.h"
#include "core/config/yaml_config_loader.h"
#include "core/engine/pipeline_controller.h"
#include "plugin/builtin/builtin_plugins.h"
#include "plugin/manager/plugin_manager.h"
#include "server/http_server.h"
#include "sinks/prometheus_exposition/prometheus_sink.h"
#include "storage/storage_backend.h"
#include "core/common/self_observability.h"

// 全局运行标志，受信号处理器控制
static std::atomic<bool> g_running{true};

// 信号处理器：收到 SIGINT/SIGTERM 时设置 g_running 为 false
static void SignalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

// 打印命令行使用帮助
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

// 设置全局日志级别
static void SetLogLevel(const std::string& level) {
    using illuminator::LogLevel;
    if (level == "trace") illuminator::Logger::Instance().SetLevel(LogLevel::kTrace);
    else if (level == "debug") illuminator::Logger::Instance().SetLevel(LogLevel::kDebug);
    else if (level == "info")  illuminator::Logger::Instance().SetLevel(LogLevel::kInfo);
    else if (level == "warn")  illuminator::Logger::Instance().SetLevel(LogLevel::kWarn);
    else if (level == "error") illuminator::Logger::Instance().SetLevel(LogLevel::kError);
}

namespace illuminator {

// ---- JSON 序列化工具 ----

// JSON 字符串转义：处理引号、反斜杠、换行、制表符等特殊字符
static std::string EscapeJson(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// FieldValue variant → JSON 字符串
static std::string FieldValueToJson(const FieldValue& fv) {
    if (auto* b = std::get_if<bool>(&fv)) return *b ? "true" : "false";
    if (auto* i = std::get_if<int64_t>(&fv)) return std::to_string(*i);
    if (auto* u = std::get_if<uint64_t>(&fv)) return std::to_string(*u);
    if (auto* d = std::get_if<double>(&fv)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f", *d);
        return buf;
    }
    if (auto* sv = std::get_if<std::string_view>(&fv))
        return "\"" + EscapeJson(*sv) + "\"";
    return "null";
}

// 堆栈帧列表 → JSON 数组字符串
std::string StackFramesToJson(const std::vector<StackFrame>& frames) {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& f : frames) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"address\":" << f.address;
        if (!f.function_name.empty())
            ss << ",\"function_name\":\"" << EscapeJson(f.function_name) << "\"";
        if (!f.module_name.empty())
            ss << ",\"module_name\":\"" << EscapeJson(f.module_name) << "\"";
        ss << "}";
    }
    ss << "]";
    return ss.str();
}

// 完整 DataBatch → JSON 字符串
// 包括 pipeline 名称、records 数组（labels + fields）、
// 以及可选的 stack_samples 数组（含 kernel_stack + user_stack）
std::string BatchToJson(const DataBatch& batch, const std::string& pipeline) {
    std::ostringstream ss;
    ss << "{\"pipeline\":\"" << EscapeJson(pipeline) << "\",\"records\":[";
    bool first = true;
    for (auto& rec : batch.records()) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"labels\":{";
        bool lf = true;
        for (auto& l : rec.labels) {
            if (!lf) ss << ",";
            lf = false;
            ss << "\"" << EscapeJson(l.key) << "\":\"" << EscapeJson(l.value) << "\"";
        }
        ss << "},\"fields\":{";
        bool ff = true;
        for (auto& [k, v] : rec.fields) {
            if (!ff) ss << ",";
            ff = false;
            ss << "\"" << EscapeJson(k) << "\":" << FieldValueToJson(v);
        }
        ss << "}}";
    }
    ss << "]";

    // 堆栈采样部分（如果存在）
    auto& samples = batch.stack_samples();
    if (!samples.empty()) {
        ss << ",\"stack_samples\":[";
        bool sf = true;
        for (auto& s : samples) {
            if (!sf) ss << ",";
            sf = false;
            ss << "{\"pid\":" << s.pid
               << ",\"tid\":" << s.tid
               << ",\"cpu\":" << s.cpu
               << ",\"count\":" << s.count
               << ",\"comm\":\"" << EscapeJson(s.comm) << "\""
               << ",\"type\":" << static_cast<int>(s.sample_type)
               << ",\"kernel_stack\":" << StackFramesToJson(s.kernel_stack)
               << ",\"user_stack\":" << StackFramesToJson(s.user_stack)
               << "}";
        }
        ss << "]";
    }

    ss << "}";
    return ss.str();
}

}  // namespace illuminator

// ========================================================================
// BuildDemoConfig — 构建内置演示配置
// ========================================================================
// 当用户未指定配置文件时，自动创建 4 条默认 Pipeline 继续运行
static illuminator::GlobalConfig BuildDemoConfig() {
    illuminator::GlobalConfig config;
    config.log_level = "info";

    // Pipeline 1: 系统 CPU 利用率（新版统一源）
    {
        illuminator::PipelineConfig pc;
        pc.name = "cpu_utilization";
        pc.source.type = "cpu_utilization";
        pc.source.config["interval_ms"] = int64_t{1000};
        pc.source.config["collect_per_core"] = "true";
        pc.source.config["collect_frequency"] = "true";
        pc.source.config["ema_alpha"] = "0.3";

        illuminator::ConfigValue storage_cfg;
        storage_cfg["backend"] = "sqlite";
        storage_cfg["path"] = "/tmp/illuminator_data";
        storage_cfg["pipeline"] = "cpu_utilization";
        pc.sinks.push_back({"local_storage", storage_cfg});

        config.pipelines.push_back(std::move(pc));
    }

    // Pipeline 2: 进程 CPU 监控（新版源，Top-N + 过滤）
    {
        illuminator::PipelineConfig pc;
        pc.name = "cpu_processes";
        pc.source.type = "process_cpu";
        pc.source.config["interval_ms"] = int64_t{2000};
        pc.source.config["top_n"] = int64_t{50};
        pc.source.config["thread_detail_threshold_pct"] = "3.0";

        illuminator::ConfigValue storage_cfg;
        storage_cfg["backend"] = "sqlite";
        storage_cfg["path"] = "/tmp/illuminator_data";
        storage_cfg["pipeline"] = "cpu_processes";
        pc.sinks.push_back({"local_storage", storage_cfg});

        config.pipelines.push_back(std::move(pc));
    }

    // Pipeline 3: CPU 性能剖析（eBPF 采样 → 符号化 → 堆栈合并）
    {
        illuminator::PipelineConfig pc;
        pc.name = "cpu_profile";
        pc.source.type = "cpu_profiler";
        pc.source.config["frequency_hz"] = int64_t{49};
        pc.source.config["mode"] = "aggregated";
        pc.source.config["user_stacks"] = "true";
        pc.source.config["kernel_stacks"] = "true";

        // Processor 1: 堆栈符号化（地址 → 函数名）
        illuminator::PipelineConfig::StageConfig sym_cfg;
        sym_cfg.type = "stack_symbolizer";
        sym_cfg.config["demangle"] = "true";
        sym_cfg.config["kernel_symbols"] = "true";
        pc.processors.push_back(sym_cfg);

        // Processor 2: 堆栈合并（相同调用栈计数累加）
        illuminator::PipelineConfig::StageConfig merge_cfg;
        merge_cfg.type = "stack_merger";
        merge_cfg.config["group_by"] = "comm";
        merge_cfg.config["include_kernel"] = "true";
        pc.processors.push_back(merge_cfg);

        // 双 Sink: 本地存储 + pprof 导出
        illuminator::ConfigValue storage_cfg;
        storage_cfg["backend"] = "sqlite";
        storage_cfg["path"] = "/tmp/illuminator_data";
        storage_cfg["pipeline"] = "cpu_profile";
        pc.sinks.push_back({"local_storage", storage_cfg});
        pc.sinks.push_back({"pprof_export", illuminator::ConfigValue()});

        config.pipelines.push_back(std::move(pc));
    }

    // Pipeline 4: 调度器分析
    {
        illuminator::PipelineConfig pc;
        pc.name = "sched_analysis";
        pc.source.type = "sched_analyzer";
        pc.source.config["detailed_mode"] = "false";
        pc.source.config["aggregate_interval_ms"] = int64_t{5000};
        pc.source.config["track_migrations"] = "true";

        illuminator::ConfigValue storage_cfg;
        storage_cfg["backend"] = "sqlite";
        storage_cfg["path"] = "/tmp/illuminator_data";
        storage_cfg["pipeline"] = "sched_analysis";
        pc.sinks.push_back({"local_storage", storage_cfg});

        config.pipelines.push_back(std::move(pc));
    }

    return config;
}

// ========================================================================
// RunDaemon — 启动守护进程模式
// ========================================================================
// 完整的启动流程：
//   插件注册 → 配置加载 → 管道构建 → 管道启动 → HTTP 服务器启动 → 接收信号
static int RunDaemon(const std::string& config_path, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Illuminator v0.1.0 starting...");

    // 强制链接所有内置插件 + 打印当前注册信息
    illuminator::RegisterBuiltinPlugins();
    illuminator::PluginManager::Instance().PrintRegisteredPlugins();

    // 加载配置
    illuminator::GlobalConfig config;
    if (!config_path.empty()) {
        IL_INFO("Loading configuration from: %s", config_path.c_str());
        auto result = illuminator::YamlConfigLoader::LoadFromFile(config_path);
        if (!result.ok()) {
            IL_ERROR("Failed to load config: %s", result.status().message().c_str());
            return 1;
        }
        config = result.value();
        if (!config.log_level.empty()) SetLogLevel(config.log_level);
    } else {
        IL_INFO("No config file specified, using built-in demo configuration");
        config = BuildDemoConfig();
    }

    // 构建所有管道
    illuminator::PipelineController controller;
    auto status = controller.BuildFromConfig(config);
    if (!status.ok()) {
        IL_ERROR("Failed to build pipelines: %s", status.message().c_str());
        return 1;
    }

    // 启动所有管道（任一条失败则停止已启动的管道）
    status = controller.StartAll();
    if (!status.ok()) {
        IL_ERROR("Failed to start pipelines: %s", status.message().c_str());
        return 1;
    }

    // ---- 启动 HTTP 服务器（注册所有 API 端点） ----

    illuminator::HttpServer http_server;

    // 健康检查端点
    http_server.RegisterHandler("/healthz", [](const std::string&) {
        return "{\"status\":\"ok\",\"version\":\"0.1.0\"}\n";
    });

    // 管道状态列表端点
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

    // CPU 利用率端点（新版统一源，返回结构化 JSON）
    http_server.RegisterHandler("/api/v1/cpu/utilization",
        [&controller](const std::string&) {
        auto* pipe = controller.GetPipeline("cpu_utilization");
        if (!pipe) return std::string("{\"error\":\"cpu_utilization pipeline not found\"}\n");
        auto* source = pipe->GetSource();
        if (!source) return std::string("{\"error\":\"no source\"}\n");
        auto result = source->Collect();
        if (!result.ok()) return std::string("{\"error\":\"collect failed\"}\n");
        return illuminator::BatchToJson(*result.value(), "cpu_utilization") + "\n";
    });

    // 进程 CPU 指标端点
    http_server.RegisterHandler("/api/v1/cpu/processes",
        [&controller](const std::string&) {
        auto* pipe = controller.GetPipeline("cpu_processes");
        if (!pipe) return std::string("{\"error\":\"cpu_processes pipeline not found\"}\n");
        auto* source = pipe->GetSource();
        if (!source) return std::string("{\"error\":\"no source\"}\n");
        auto result = source->Collect();
        if (!result.ok()) return std::string("{\"error\":\"collect failed\"}\n");
        return illuminator::BatchToJson(*result.value(), "cpu_processes") + "\n";
    });

    // CPU Profile / 火焰图端点（含完整 Processor 链： 符号化 + 堆栈合并）
    http_server.RegisterHandler("/api/v1/cpu/profile/flamegraph",
        [&controller](const std::string&) {
        auto* pipe = controller.GetPipeline("cpu_profile");
        if (!pipe) return std::string("{\"error\":\"cpu_profile pipeline not found\"}\n");
        auto* source = pipe->GetSource();
        if (!source) return std::string("{\"error\":\"no source\"}\n");
        auto result = source->Collect();
        if (!result.ok()) return std::string("{\"error\":\"collect failed\"}\n");
        auto processed = pipe->RunProcessors(std::move(*result));
        if (!processed.ok()) return std::string("{\"error\":\"symbolization failed\"}\n");
        return illuminator::BatchToJson(**processed, "cpu_profile") + "\n";
    });

    // 调度器摘要端点
    http_server.RegisterHandler("/api/v1/cpu/sched/summary",
        [&controller](const std::string&) {
        auto* pipe = controller.GetPipeline("sched_analysis");
        if (!pipe) return std::string("{\"error\":\"sched_analysis pipeline not found\"}\n");
        auto* source = pipe->GetSource();
        if (!source) return std::string("{\"error\":\"no source\"}\n");
        auto result = source->Collect();
        if (!result.ok()) return std::string("{\"error\":\"collect failed\"}\n");
        return illuminator::BatchToJson(*result.value(), "sched_analysis") + "\n";
    });

    // 自观测端点（Prometheus 格式 + JSON 格式）
    http_server.RegisterHandler("/metrics", [](const std::string&) {
        return illuminator::InternalMetrics::Instance().ExportPrometheus();
    });
    http_server.RegisterHandler("/api/v1/internal_metrics", [](const std::string&) {
        return illuminator::InternalMetrics::Instance().ExportJson() + "\n";
    });

    // 静态前端文件服务 + HTTP 启动
    http_server.SetStaticDir("web/dist");
    http_server.Start("0.0.0.0", 9527);

    IL_INFO("Illuminator daemon running. HTTP on :9527. Ctrl+C to stop.");

    // 注册信号处理
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 主循环：每 500ms 检查一次运行标志
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 优雅关闭
    IL_INFO("Shutting down...");
    http_server.Stop();
    controller.StopAll();
    IL_INFO("Illuminator stopped.");
    return 0;
}

// ========================================================================
// RunCollect — 一次性数据采集模式
// ========================================================================
static int RunCollect(int duration_sec, const std::string& log_level) {
    SetLogLevel(log_level);
    IL_INFO("Collecting for %d seconds...", duration_sec);

    illuminator::RegisterBuiltinPlugins();

    auto config = BuildDemoConfig();
    illuminator::PipelineController controller;
    controller.BuildFromConfig(config);
    controller.StartAll();

    // 运行指定秒数
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

    controller.StopAll();
    IL_INFO("Collection complete.");
    return 0;
}

// ========================================================================
// RunTop — 实时系统概览（类 htop 终端界面）
// ========================================================================
static int RunTop(const std::string& log_level) {
    SetLogLevel(log_level);
    illuminator::RegisterBuiltinPlugins();

    fprintf(stdout, "Illuminator Top - Press Ctrl+C to exit\n\n");
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 使用 proc_stat_reader 插件直接采集系统指标
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
        // 清屏 + 光标归位（ANSI 转义码）
        fprintf(stdout, "\033[2J\033[H");
        fprintf(stdout, "Illuminator Top - %s\n",
                batch->GetMeta("_time", "").c_str());
        fprintf(stdout, "%-10s %12s %12s %12s %12s\n",
                "CPU", "User%", "System%", "Idle%", "IOWait%");
        fprintf(stdout, "--------------------------------------------------------------\n");

        // 遍历所有 Record（每核一行）
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

// ========================================================================
// RunPluginList — 列出所有已注册插件
// ========================================================================
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

// ========================================================================
// RunStorageList — 列出所有可用存储后端
// ========================================================================
static int RunStorageList() {
    illuminator::RegisterBuiltinPlugins();
    auto names = illuminator::StorageFactory::Instance().Available();
    std::cout << "=== Available Storage Backends ===\n";
    for (auto& n : names) std::cout << "  - " << n << "\n";
    return 0;
}

// ============================================================================
// main() — 程序入口
// ============================================================================
// 解析命令行参数，路由到对应的命令处理函数
int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    std::string config_path, log_level = "info", output, format;
    int duration = 10;  // 默认采集 10 秒

    // 解析可选参数
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

    // 路由到对应命令函数
    if (command == "daemon") return RunDaemon(config_path, log_level);
    if (command == "collect") return RunCollect(duration, log_level);
    if (command == "top") return RunTop(log_level);
    if (command == "version") { std::cout << "Illuminator v0.1.0\n"; return 0; }
    if (command == "plugins") return RunPluginList();
    if (command == "storage") return RunStorageList();

    // 未知命令，打印帮助
    PrintUsage(argv[0]);
    return 1;
}
