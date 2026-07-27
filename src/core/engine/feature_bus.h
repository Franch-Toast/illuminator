// ============================================================================
// FeatureBus — Feature 驱动总线（注册、发现、生命周期编排）
// ============================================================================
//
// FeatureBus 是所有 FeatureDriver 的注册中心和编排器。
// 类似 Linux 的 struct bus_type，负责：
//   1. 维护已注册 Driver 列表
//   2. 按需启停（Probe/Remove）单个或全部 Driver
//   3. 提供全局查询接口（按 name、tier、category 筛选）
//   4. 状态变更通知（SSE 推送用）
//
// 线程安全：
//   所有公开方法都是线程安全的（内部 shared_mutex 保护）。
// ============================================================================

#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/feature_driver.h"

namespace illuminator {

class FeatureBus {
public:
    using StateChangeCallback = std::function<void(const std::string& feature,
                                                    DriverState from,
                                                    DriverState to)>;

    static FeatureBus& Instance() {
        static FeatureBus bus;
        return bus;
    }

    // Register — 注册一个 FeatureDriver（不启动）
    Status Register(std::unique_ptr<FeatureDriver> driver) {
        if (!driver) {
            return Status::Error(StatusCode::kInvalidArgument, "null driver");
        }
        std::string name = driver->Name();

        std::unique_lock lock(mu_);
        if (drivers_.count(name)) {
            return Status::Error(StatusCode::kAlreadyExists,
                                 "driver already registered: " + name);
        }
        drivers_[name] = std::shared_ptr<FeatureDriver>(std::move(driver));
        IL_INFO("FeatureBus: registered '{}'", name);
        return Status::Ok();
    }

    // Unregister — 移除一个 Driver（先 Remove 再卸载）
    Status Unregister(const std::string& name) {
        std::unique_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        auto old_state = it->second->State();
        it->second->Remove();
        drivers_.erase(it);
        lock.unlock();

        if (state_callback_ && old_state != DriverState::kInactive) {
            state_callback_(name, old_state, DriverState::kInactive);
        }
        return Status::Ok();
    }

    // Probe — 启动指定 Feature
    Status Probe(const std::string& name) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        auto old_state = it->second->State();
        auto status = it->second->Probe();
        if (status.ok() && state_callback_) {
            lock.unlock();
            state_callback_(name, old_state, DriverState::kActive);
        }
        return status;
    }

    // Remove — 停止指定 Feature
    Status Remove(const std::string& name) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        auto old_state = it->second->State();
        auto status = it->second->Remove();
        if (status.ok() && state_callback_ && old_state != DriverState::kInactive) {
            lock.unlock();
            state_callback_(name, old_state, DriverState::kInactive);
        }
        return status;
    }

    // Pause/Resume
    Status Pause(const std::string& name) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        auto old_state = it->second->State();
        auto status = it->second->Pause();
        if (status.ok() && state_callback_) {
            lock.unlock();
            state_callback_(name, old_state, DriverState::kPaused);
        }
        return status;
    }

    Status Resume(const std::string& name) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        auto old_state = it->second->State();
        auto status = it->second->Resume();
        if (status.ok() && state_callback_) {
            lock.unlock();
            state_callback_(name, old_state, DriverState::kActive);
        }
        return status;
    }

    // Reconfigure — 运行时动态更新 Feature 参数
    Status Reconfigure(const std::string& name, const ConfigValue& params) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        return it->second->Reconfigure(params);
    }

    // ProbeAll — 启动所有已注册的 Tier 1/2 Driver
    Status ProbeAll() {
        std::shared_lock lock(mu_);
        for (auto& [name, drv] : drivers_) {
            if (drv->Tier() <= DriverTier::kTracing &&
                drv->State() == DriverState::kInactive) {
                auto status = drv->Probe();
                if (!status.ok()) {
                    IL_WARN("FeatureBus: failed to probe '{}': {}", name, status.message());
                }
            }
        }
        return Status::Ok();
    }

    // RemoveAll — 停止所有 Driver
    Status RemoveAll() {
        std::shared_lock lock(mu_);
        for (auto& [name, drv] : drivers_) {
            drv->Remove();
        }
        return Status::Ok();
    }

    // GetDriver — 获取 Driver 引用（只读查询）
    std::shared_ptr<FeatureDriver> GetDriver(const std::string& name) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        return (it != drivers_.end()) ? it->second : nullptr;
    }

    // ListDrivers — 列出所有 Driver 的运行时信息
    std::vector<DriverInfo> ListDrivers() const {
        std::shared_lock lock(mu_);
        std::vector<DriverInfo> result;
        result.reserve(drivers_.size());
        for (auto& [name, drv] : drivers_) {
            result.push_back(drv->Info());
        }
        return result;
    }

    // ListDescriptors — 列出所有 Driver 的自描述信息（前端自动发现）
    std::vector<FeatureDescriptor> ListDescriptors() const {
        std::shared_lock lock(mu_);
        std::vector<FeatureDescriptor> result;
        result.reserve(drivers_.size());
        for (auto& [name, drv] : drivers_) {
            result.push_back(drv->Describe());
        }
        return result;
    }

    // GetConfigSchema — 获取指定 Feature 的 JSON Schema
    std::string GetConfigSchema(const std::string& name) const {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) return "";
        return it->second->ConfigSchema();
    }

    // GetConfig — 获取指定 Feature 的当前配置
    std::string GetConfig(const std::string& name) const {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) return "";
        return it->second->GetConfig();
    }

    // SetConfig — 更新指定 Feature 的配置
    Status SetConfig(const std::string& name, const std::string& json_config) {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) {
            return Status::Error(StatusCode::kNotFound, "driver not found: " + name);
        }
        return it->second->SetConfig(json_config);
    }

    // GetStats — 获取指定 Feature 的运行统计
    FeatureStats GetStats(const std::string& name) const {
        std::shared_lock lock(mu_);
        auto it = drivers_.find(name);
        if (it == drivers_.end()) return {};
        return it->second->GetStats();
    }

    size_t Size() const {
        std::shared_lock lock(mu_);
        return drivers_.size();
    }

    void SetStateChangeCallback(StateChangeCallback cb) {
        state_callback_ = std::move(cb);
    }

private:
    FeatureBus() = default;
    ~FeatureBus() { RemoveAll(); }

    FeatureBus(const FeatureBus&) = delete;
    FeatureBus& operator=(const FeatureBus&) = delete;

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<FeatureDriver>> drivers_;
    StateChangeCallback state_callback_;
};

}  // namespace illuminator
