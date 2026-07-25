// ============================================================================
// Illuminator 插件管理器 — 插件发现、加载和生命周期管理
// ============================================================================
//
// PluginManager 是插件系统的顶层管理类，负责：
// 1. 从配置的目录中扫描并加载 .so 动态库插件
// 2. 提供已注册插件的查看接口
//
// 与 PluginRegistry 的关系：
// ===========================
// - PluginRegistry 负责插件的"注册"和"创建"
// - PluginManager 负责插件的"发现"和"加载"
// - 内置插件通过静态初始化自动注入 PluginRegistry
// - .so 插件由 PluginManager 发现后注入 PluginRegistry
//
// 加载流程：
// ==========
// main() → RegisterBuiltinPlugins() → 加载配置文件 →
//   → PluginManager::LoadPluginsFromDirs(config.plugin_dirs) →
//     → SoLoader::LoadPlugin(每个 .so) → 通过 C ABI 获取描述符
// ============================================================================

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "plugin/infra/plugin_registry.h"
#include "plugin/infra/so_loader.h"

namespace illuminator {

class PluginManager {
public:
    static PluginManager& Instance() {
        static PluginManager instance;
        return instance;
    }

    // ---- 从目录加载 .so 插件 ----
    // 遍历指定目录中的所有 .so 文件，调用 SoLoader 加载并注册到 PluginRegistry。
    // 不存在的目录会被跳过并记录警告。
    Status LoadPluginsFromDirs(const std::vector<std::string>& dirs) {
        if (dirs.empty()) return Status::Ok();
        so_loader_.SetAllowedDirs(dirs);

        size_t before_count = so_loader_.Descriptors().size();
        for (auto& dir : dirs) {
            if (!std::filesystem::exists(dir)) {
                IL_WARN("Plugin directory does not exist: {}", dir);
                continue;
            }
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().extension() == ".so") {
                    auto status = so_loader_.LoadPlugin(entry.path().string());
                    if (!status.ok()) {
                        IL_ERROR("Failed to load plugin {}: {}",
                                 entry.path().string(), status.message());
                    }
                }
            }
        }

        auto& reg = PluginRegistry::Instance();
        auto& descriptors = so_loader_.Descriptors();
        for (size_t i = before_count; i < descriptors.size(); ++i) {
            BridgeDescriptorToRegistry(descriptors[i], reg);
        }
        return Status::Ok();
    }

    // ---- 访问器 ----
    PluginRegistry& Registry() { return PluginRegistry::Instance(); }
    const SoLoader& Loader() const { return so_loader_; }

    // ---- 打印已注册插件列表 ----
    // 用于启动日志和 `illuminator plugins` 命令
    void PrintRegisteredPlugins() const {
        auto& reg = PluginRegistry::Instance();
        IL_INFO("=== Registered Plugins ===");
        for (auto& n : reg.ListSources())     IL_INFO("  Source:     {}", n);
        for (auto& n : reg.ListProcessors())  IL_INFO("  Processor:  {}", n);
        for (auto& n : reg.ListAggregators()) IL_INFO("  Aggregator: {}", n);
        for (auto& n : reg.ListSinks())       IL_INFO("  Sink:       {}", n);
    }

private:
    PluginManager() = default;

    static void BridgeDescriptorToRegistry(const IlPluginDescriptor* desc,
                                           PluginRegistry& reg) {
        if (!desc || !desc->create || !desc->name) return;
        auto plugin_type = static_cast<PluginType>(desc->type);
        std::string name(desc->name);

        switch (plugin_type) {
        case PluginType::kSource:
            reg.RegisterSource(name, [desc]() {
                void* raw = desc->create(nullptr);
                return std::unique_ptr<SourcePlugin>(
                    static_cast<SourcePlugin*>(raw));
            });
            break;
        case PluginType::kProcessor:
            reg.RegisterProcessor(name, [desc]() {
                void* raw = desc->create(nullptr);
                return std::unique_ptr<ProcessorPlugin>(
                    static_cast<ProcessorPlugin*>(raw));
            });
            break;
        case PluginType::kAggregator:
            reg.RegisterAggregator(name, [desc]() {
                void* raw = desc->create(nullptr);
                return std::unique_ptr<AggregatorPlugin>(
                    static_cast<AggregatorPlugin*>(raw));
            });
            break;
        case PluginType::kSink:
            reg.RegisterSink(name, [desc]() {
                void* raw = desc->create(nullptr);
                return std::unique_ptr<SinkPlugin>(
                    static_cast<SinkPlugin*>(raw));
            });
            break;
        }
        IL_INFO("Bridged SO plugin '{}' -> PluginRegistry (type={})",
                name, PluginTypeToString(plugin_type));
    }

    SoLoader so_loader_;
};

}  // namespace illuminator
