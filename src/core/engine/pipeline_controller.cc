// ============================================================================
// PipelineController 实现 — 从配置构建并管理多条数据处理管道
// ============================================================================
//
// BuildFromConfig(GlobalConfig):
//   这是系统初始化的核心方法。遍历配置中的每条管道定义，完成：
//   1. 从 PluginRegistry 查找并实例化 Source/Processor/Aggregator/Sink 插件
//   2. 调用每个插件的 Init() 方法，传入配置参数
//   3. 将各插件按正确顺序组装到 Pipeline 对象中
//
//   对于每条管道的四个阶段，处理逻辑如下：
//   - Source: 必须存在，且管道必须有 Source
//   - Processor: 可选的多个处理器，按配置顺序串联
//   - Aggregator: 可选的时间窗口聚合器
//   - Sink: 至少一个数据出口，每个管道可以配置多个 Sink
//
// 示例：一条 CPU Profiling 管道的构建过程：
//   Pipeline("cpu_profile")
//     → Source("cpu_profiler")       // eBPF CPU 采样
//     → Processor("stack_symbolizer") // 符号化堆栈地址
//     → Processor("stack_merger")     // 合并相同堆栈
//     → Sink("local_storage")         // 存入 SQLite
//     → Sink("pprof_export")          // 导出 pprof 格式
// ============================================================================

#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

Status PipelineController::BuildFromConfig(const GlobalConfig& config) {
    auto& registry = PluginRegistry::Instance();

    // 遍历配置中的每条管道定义
    for (auto& pc : config.pipelines) {
        auto pipeline = std::make_unique<Pipeline>(pc.name);

        // ====================================================================
        // 第一步：创建并配置 Source（数据源）
        // ====================================================================
        auto source = registry.CreateSource(pc.source.type);
        if (!source) {
            // 如果找不到指定类型的插件，返回明确的错误信息
            return Status::Error(StatusCode::kNotFound,
                "Source plugin not found: " + pc.source.type +
                " (pipeline: " + pc.name + ")");
        }
        // 用配置参数初始化 Source 插件
        auto status = source->Init(pc.source.config);
        if (!status.ok()) return status;
        pipeline->SetSource(std::move(source));

        // ====================================================================
        // 第二步：创建并配置 Processor 链
        // ====================================================================
        for (auto& proc_cfg : pc.processors) {
            auto proc = registry.CreateProcessor(proc_cfg.type);
            if (!proc) {
                return Status::Error(StatusCode::kNotFound,
                    "Processor plugin not found: " + proc_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = proc->Init(proc_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddProcessor(std::move(proc));
        }

        // ====================================================================
        // 第三步：创建并配置 Aggregator（可选）
        // ====================================================================
        if (pc.aggregator.has_value()) {
            auto agg = registry.CreateAggregator(pc.aggregator->type);
            if (!agg) {
                return Status::Error(StatusCode::kNotFound,
                    "Aggregator plugin not found: " + pc.aggregator->type +
                    " (pipeline: " + pc.name + ")");
            }
            status = agg->Init(pc.aggregator->config);
            if (!status.ok()) return status;
            pipeline->SetAggregator(std::move(agg));
        }

        // ====================================================================
        // 第四步：创建并配置 Sink 列表
        // ====================================================================
        for (auto& sink_cfg : pc.sinks) {
            auto sink = registry.CreateSink(sink_cfg.type);
            if (!sink) {
                return Status::Error(StatusCode::kNotFound,
                    "Sink plugin not found: " + sink_cfg.type +
                    " (pipeline: " + pc.name + ")");
            }
            status = sink->Init(sink_cfg.config);
            if (!status.ok()) return status;
            pipeline->AddSink(std::move(sink));
        }

        IL_INFO("Built pipeline: {}", pc.name);
        pipelines_.push_back(std::move(pipeline));
    }

    // 所有管道构建完成后，初始化共享 Sink 线程池
    size_t sink_threads = config.engine.sink_pool_threads;
    InitSinkPool(sink_threads);

    return Status::Ok();
}

Status PipelineController::StartAll() {
    for (auto& pipeline : pipelines_) {
        auto status = pipeline->Start();
        if (!status.ok()) {
            IL_ERROR("Failed to start pipeline '{}': {}",
                     pipeline->name(), status.message());
            StopAll();  // 回滚：停止已启动的管道
            return status;
        }
    }
    IL_INFO("All {} pipelines started", pipelines_.size());
    return Status::Ok();
}

// ---- 停止所有管道 ----
// 依次调用每条管道的 Stop() 方法
Status PipelineController::StopAll() {
    for (auto& pipeline : pipelines_) {
        pipeline->Stop();
    }
    IL_INFO("All pipelines stopped");
    return Status::Ok();
}

// ---- 按名称查找管道 ----
// 线性搜索（管道数量通常很少，n 在个位数到几十之间）
Pipeline* PipelineController::GetPipeline(const std::string& name) {
    for (auto& p : pipelines_) {
        if (p->name() == name) return p.get();
    }
    return nullptr;  // 未找到
}

}  // namespace illuminator
