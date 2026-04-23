#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "plugin/manager/plugin_registry.h"
#include "plugin/manager/so_loader.h"

namespace illuminator {

// PluginManager: discovers, loads, and manages all plugins.
// Handles builtin registration and runtime SO loading.
class PluginManager {
public:
    static PluginManager& Instance() {
        static PluginManager instance;
        return instance;
    }

    // Scan plugin directories for .so files and load them
    Status LoadPluginsFromDirs(const std::vector<std::string>& dirs) {
        for (auto& dir : dirs) {
            if (!std::filesystem::exists(dir)) {
                IL_WARN("Plugin directory does not exist: %s", dir.c_str());
                continue;
            }
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

    PluginRegistry& Registry() { return PluginRegistry::Instance(); }
    const SoLoader& Loader() const { return so_loader_; }

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
    SoLoader so_loader_;
};

}  // namespace illuminator
