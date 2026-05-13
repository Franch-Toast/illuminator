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
#include "plugin/manager/plugin_registry.h"
#include "plugin/manager/so_loader.h"

namespace illuminator {

class PluginManager {
public:
    static PluginManager& Instance() {
        static PluginManager instance;
        return instance;
    }

    // ---- 从目录加载 .so 插件 ----
    // 遍历指定目录中的所有 .so 文件，调用 SoLoader 加载并注册。
    // 不存在的目录会被跳过并记录警告。
    Status LoadPluginsFromDirs(const std::vector<std::string>& dirs) {
        for (auto& dir : dirs) {
            // 检查目录是否存在，避免不必要的错误
            if (!std::filesystem::exists(dir)) {
                IL_WARN("Plugin directory does not exist: %s", dir.c_str());
                continue;
            }
            // 遍历目录，只处理 .so 文件
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().extension() == ".so") {
                    auto status = so_loader_.LoadPlugin(entry.path().string());
                    if (!status.ok()) {
                        IL_ERROR("Failed to load plugin %s: %s",
                                 entry.path().c_str(), status.message().c_str());
                    }
                }
            }
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
        for (auto& n : reg.ListSources())     IL_INFO("  Source:     %s", n.c_str());
        for (auto& n : reg.ListProcessors())  IL_INFO("  Processor:  %s", n.c_str());
        for (auto& n : reg.ListAggregators()) IL_INFO("  Aggregator: %s", n.c_str());
        for (auto& n : reg.ListSinks())       IL_INFO("  Sink:       %s", n.c_str());
    }

private:
    PluginManager() = default;
    SoLoader so_loader_;  // .so 动态库加载器
};

}  // namespace illuminator
