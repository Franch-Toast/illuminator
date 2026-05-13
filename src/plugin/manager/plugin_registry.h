// ============================================================================
// Illuminator 插件注册表 — 集中式工厂模式
// ============================================================================
//
// PluginRegistry 是 Illuminator 的插件注册和发现中心。
// 采用"注册表 + 工厂函数"模式，实现插件系统的解耦和扩展性。
//
// 核心机制：
// ==========
// 1. 工厂函数注册
//    - 每个插件向注册表注册一个"工厂函数"（lambda / function object）
//    - 工厂函数在调用时创建一个新的插件实例（unique_ptr）
//    - 注册表只存储工厂函数，不持有插件实例
//
// 2. 四种插件类型的四个独立注册表
//    - sources_:     数据源工厂映射表
//    - processors_:   处理器工厂映射表
//    - aggregators_:  聚合器工厂映射表
//    - sinks_:        数据出口工厂映射表
//
// 3. 线程安全
//    - 所有注册和创建操作都使用 mutex_ 保护
//    - 支持并发注册（静态初始化阶段）和并发查询（运行阶段）
//
// 4. 自动注册宏
//    - IL_REGISTER_SOURCE(name, cls)      — 注册数据源
//    - IL_REGISTER_PROCESSOR(name, cls)   — 注册处理器
//    - IL_REGISTER_AGGREGATOR(name, cls)  — 注册聚合器
//    - IL_REGISTER_SINK(name, cls)        — 注册数据出口
//    这些宏利用 C++ 静态初始化器（static initializer），在 main() 之前
//    自动执行注册 lambda，无需手动调用注册函数。
//
// 使用示例：
// ==========
// 在插件头文件末尾添加一行：
//   IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);
// 这会生成一个静态变量，其初始化 lambda 在程序启动时自动将
// "cpu_utilization" 与 CpuUtilizationSource 的工厂函数关联。
// ============================================================================

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/common/logging.h"
#include "plugin/api/plugin_api.h"
#include "plugin/api/source_plugin.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/api/aggregator_plugin.h"
#include "plugin/api/sink_plugin.h"

namespace illuminator {

// ---- 工厂函数类型 ----
// 每种插件类型有对应的工厂函数签名，
// 调用工厂函数时创建一个对应类型的插件实例并返回 unique_ptr
using SourceFactory     = std::function<std::unique_ptr<SourcePlugin>()>;
using ProcessorFactory  = std::function<std::unique_ptr<ProcessorPlugin>()>;
using AggregatorFactory = std::function<std::unique_ptr<AggregatorPlugin>()>;
using SinkFactory       = std::function<std::unique_ptr<SinkPlugin>()>;

// ---- PluginRegistry 注册表 ----
class PluginRegistry {
public:
    // 单例访问
    static PluginRegistry& Instance() {
        static PluginRegistry instance;
        return instance;
    }

    // ==================================================================
    // 注册方法：将插件名称与工厂函数关联
    // ==================================================================

    void RegisterSource(const std::string& name, SourceFactory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        sources_[name] = std::move(factory);
        IL_DEBUG("Registered source plugin: %s", name.c_str());
    }

    void RegisterProcessor(const std::string& name, ProcessorFactory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        processors_[name] = std::move(factory);
        IL_DEBUG("Registered processor plugin: %s", name.c_str());
    }

    void RegisterAggregator(const std::string& name, AggregatorFactory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        aggregators_[name] = std::move(factory);
        IL_DEBUG("Registered aggregator plugin: %s", name.c_str());
    }

    void RegisterSink(const std::string& name, SinkFactory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_[name] = std::move(factory);
        IL_DEBUG("Registered sink plugin: %s", name.c_str());
    }

    // ==================================================================
    // 创建方法：根据名称查找并调用工厂函数创建插件实例
    // ==================================================================
    // 返回 nullptr 表示未找到该名称的插件

    std::unique_ptr<SourcePlugin> CreateSource(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sources_.find(name);
        if (it == sources_.end()) return nullptr;
        return it->second();  // 调用工厂函数，返回新创建的实例
    }

    std::unique_ptr<ProcessorPlugin> CreateProcessor(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = processors_.find(name);
        if (it == processors_.end()) return nullptr;
        return it->second();
    }

    std::unique_ptr<AggregatorPlugin> CreateAggregator(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = aggregators_.find(name);
        if (it == aggregators_.end()) return nullptr;
        return it->second();
    }

    std::unique_ptr<SinkPlugin> CreateSink(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(name);
        if (it == sinks_.end()) return nullptr;
        return it->second();
    }

    // ==================================================================
    // 列表方法：列出所有已注册的插件名称
    // ==================================================================
    std::vector<std::string> ListSources() const { return ListKeys(sources_); }
    std::vector<std::string> ListProcessors() const { return ListKeys(processors_); }
    std::vector<std::string> ListAggregators() const { return ListKeys(aggregators_); }
    std::vector<std::string> ListSinks() const { return ListKeys(sinks_); }

private:
    PluginRegistry() = default;

    // 辅助函数：提取 map 中所有 key
    template <typename M>
    static std::vector<std::string> ListKeys(const M& m) {
        std::vector<std::string> keys;
        keys.reserve(m.size());
        for (auto& [k, _] : m) keys.push_back(k);
        return keys;
    }

    mutable std::mutex mutex_;                                  // 保护所有注册表的互斥锁
    std::unordered_map<std::string, SourceFactory> sources_;     // 数据源工厂映射
    std::unordered_map<std::string, ProcessorFactory> processors_; // 处理器工厂映射
    std::unordered_map<std::string, AggregatorFactory> aggregators_; // 聚合器工厂映射
    std::unordered_map<std::string, SinkFactory> sinks_;         // 数据出口工厂映射
};

// ==================================================================
// 自动注册宏
// ==================================================================
// 这些宏利用 C++ 静态初始化机制，在程序启动时自动注册内置插件。
//
// 工作原理：
//   static bool _il_reg_xxx = [] { Registry::Register(...); return true; }();
//   1. 创建一个静态布尔变量
//   2. 初始化器是一个立即执行的 lambda
//   3. Lambda 在静态初始化阶段执行（main() 之前）
//   4. 通过命名拼接（_il_reg_src_##cls）确保每个插件有唯一的变量名
//
// 使用方法：在插件类定义之后添加一行注册宏
//   例：IL_REGISTER_SOURCE("cpu_utilization", CpuUtilizationSource);

#define IL_REGISTER_SOURCE(name, cls) \
    static bool _il_reg_src_##cls = [] { \
        ::illuminator::PluginRegistry::Instance().RegisterSource( \
            name, [] { return std::make_unique<cls>(); }); \
        return true; \
    }()

#define IL_REGISTER_PROCESSOR(name, cls) \
    static bool _il_reg_proc_##cls = [] { \
        ::illuminator::PluginRegistry::Instance().RegisterProcessor( \
            name, [] { return std::make_unique<cls>(); }); \
        return true; \
    }()

#define IL_REGISTER_AGGREGATOR(name, cls) \
    static bool _il_reg_agg_##cls = [] { \
        ::illuminator::PluginRegistry::Instance().RegisterAggregator( \
            name, [] { return std::make_unique<cls>(); }); \
        return true; \
    }()

#define IL_REGISTER_SINK(name, cls) \
    static bool _il_reg_sink_##cls = [] { \
        ::illuminator::PluginRegistry::Instance().RegisterSink( \
            name, [] { return std::make_unique<cls>(); }); \
        return true; \
    }()

}  // namespace illuminator
