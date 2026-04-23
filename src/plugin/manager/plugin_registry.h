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

// Factory function types
using SourceFactory     = std::function<std::unique_ptr<SourcePlugin>()>;
using ProcessorFactory  = std::function<std::unique_ptr<ProcessorPlugin>()>;
using AggregatorFactory = std::function<std::unique_ptr<AggregatorPlugin>()>;
using SinkFactory       = std::function<std::unique_ptr<SinkPlugin>()>;

// Central registry for all plugin factories.
// Builtin plugins register at static-init time; SO plugins register when loaded.
class PluginRegistry {
public:
    static PluginRegistry& Instance() {
        static PluginRegistry instance;
        return instance;
    }

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

    std::unique_ptr<SourcePlugin> CreateSource(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sources_.find(name);
        if (it == sources_.end()) return nullptr;
        return it->second();
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

    std::vector<std::string> ListSources() const { return ListKeys(sources_); }
    std::vector<std::string> ListProcessors() const { return ListKeys(processors_); }
    std::vector<std::string> ListAggregators() const { return ListKeys(aggregators_); }
    std::vector<std::string> ListSinks() const { return ListKeys(sinks_); }

private:
    PluginRegistry() = default;

    template <typename M>
    static std::vector<std::string> ListKeys(const M& m) {
        std::vector<std::string> keys;
        keys.reserve(m.size());
        for (auto& [k, _] : m) keys.push_back(k);
        return keys;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SourceFactory> sources_;
    std::unordered_map<std::string, ProcessorFactory> processors_;
    std::unordered_map<std::string, AggregatorFactory> aggregators_;
    std::unordered_map<std::string, SinkFactory> sinks_;
};

// Macros for convenient builtin plugin registration
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
