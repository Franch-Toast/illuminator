// ============================================================================
// Feature Registry — Feature 编排器
// ============================================================================
//
// FeatureRegistry 管理 FeatureDriver 的注册与批量 Probe（RegisterAll）。
// 与 PluginRegistry 的职责分离是有意为之：
//   - FeatureRegistry：编排层，负责 Driver 生命周期（注册 → 实例化 → 挂到 FeatureBus）
//   - PluginRegistry：工厂 + 内省层，负责插件发现与外部 .so 加载后的实例化
//
// FeatureDriver 在 BuildPipeline() 中直接构造具体组件类型，不经过 PluginRegistry。
//
// 使用方式：
//   在 Driver 实现文件底部调用宏：
//     REGISTER_FEATURE(CpuUtilizationDriver);
//
//   然后在 main() 中调用：
//     FeatureRegistry::RegisterAll();
//
//   所有通过宏注册的 Driver 会自动实例化并注册到 FeatureBus。
// ============================================================================

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/engine/feature_bus.h"
#include "core/engine/feature_driver.h"

namespace illuminator {

class FeatureRegistry {
public:
    using DriverFactory = std::function<std::unique_ptr<FeatureDriver>()>;

    static FeatureRegistry& Instance() {
        static FeatureRegistry reg;
        return reg;
    }

    void Add(DriverFactory factory) {
        factories_.push_back(std::move(factory));
    }

    // 实例化所有已注册的 Driver 并注册到 FeatureBus
    static void RegisterAll() {
        auto& reg = Instance();
        auto& bus = FeatureBus::Instance();
        for (auto& factory : reg.factories_) {
            auto driver = factory();
            if (driver) {
                bus.Register(std::move(driver));
            }
        }
        IL_INFO("FeatureRegistry: registered {} drivers to FeatureBus",
                reg.factories_.size());
    }

private:
    FeatureRegistry() = default;
    std::vector<DriverFactory> factories_;
};

namespace detail {

struct FeatureRegistrar {
    explicit FeatureRegistrar(FeatureRegistry::DriverFactory factory) {
        FeatureRegistry::Instance().Add(std::move(factory));
    }
};

}  // namespace detail

}  // namespace illuminator

#define REGISTER_FEATURE(DriverClass) \
    static ::illuminator::detail::FeatureRegistrar \
        _reg_##DriverClass([] { \
            return std::make_unique<DriverClass>(); \
        })
