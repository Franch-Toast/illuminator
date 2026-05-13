// ============================================================================
// Illuminator SoLoader — 动态库（.so）插件加载器
// ============================================================================
//
// SoLoader 负责在运行时通过 dlopen/dlsym 加载共享库（.so）格式的插件。
// 这使得用户可以开发和部署独立的插件动态库，无需重新编译主程序。
//
// 加载流程：
// ==========
// 1. dlopen(path, RTLD_NOW | RTLD_LOCAL)
//    - RTLD_NOW: 立即解析所有符号（而非懒加载），可以在加载时发现缺失符号
//    - RTLD_LOCAL: 插件符号不对外暴露，避免符号冲突
//
// 2. dlsym(handle, "illuminator_plugin_describe")
//    - 查找约定的导出函数 illuminator_plugin_describe
//    - 该函数返回 IlPluginDescriptor* 描述插件信息
//
// 3. 保存 handle（用于后续 dlclose）和 descriptor
//
// 生命周期管理：
// ==============
// SoLoader 析构时自动 dlclose 所有加载的库，防止内存泄漏。
// ============================================================================

#pragma once

#include <dlfcn.h>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "plugin/api/plugin_api.h"

namespace illuminator {

class SoLoader {
public:
    // 析构时释放所有加载的共享库
    ~SoLoader() {
        for (auto* handle : handles_) {
            if (handle) dlclose(handle);
        }
    }

    // ---- 加载单个 .so 插件 ----
    // 参数: path — .so 文件的完整路径
    // 返回: 成功返回 Ok，失败返回详细错误信息
    Status LoadPlugin(const std::string& path) {
        // 第一步：打开共享库
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            return Status::Error(StatusCode::kInternal,
                std::string("dlopen failed: ") + dlerror());
        }

        // 第二步：查找导出符号 illuminator_plugin_describe
        auto describe_fn = reinterpret_cast<IlPluginDescribeFn>(
            dlsym(handle, IL_PLUGIN_EXPORT_NAME));
        if (!describe_fn) {
            dlclose(handle);  // 找不到符号，立即释放
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("Symbol not found: ") + IL_PLUGIN_EXPORT_NAME);
        }

        // 第三步：调用 describe 函数获取插件描述符
        const IlPluginDescriptor* desc = describe_fn();
        if (!desc) {
            dlclose(handle);  // 描述符为空，立即释放
            return Status::Error(StatusCode::kInternal,
                "Plugin describe() returned null");
        }

        // 加载成功，记录信息
        IL_INFO("Loaded SO plugin: %s v%s (type=%u) from %s",
                desc->name, desc->version, desc->type, path.c_str());

        handles_.push_back(handle);         // 保存 handle 供后续释放
        descriptors_.push_back(desc);       // 保存描述符
        return Status::Ok();
    }

    // ---- 获取所有已加载插件的描述符 ----
    const std::vector<const IlPluginDescriptor*>& Descriptors() const {
        return descriptors_;
    }

private:
    std::vector<void*> handles_;                          // dlopen 返回的句柄列表
    std::vector<const IlPluginDescriptor*> descriptors_;  // 插件描述符列表
};

}  // namespace illuminator
