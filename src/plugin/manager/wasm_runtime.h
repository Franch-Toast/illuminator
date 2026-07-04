// ============================================================================
// Illuminator WASM 运行时 — WebAssembly 沙箱插件支持
// ============================================================================
//
// WasmRuntime 提供了 WebAssembly（WASM）插件的加载和运行框架。
// 这是一个"沙箱化"的插件托管方案，适合不可信第三方插件。
//
// 当前状态：框架占位（Stub）
// ==========================
// 本版本的 WasmRuntime 只实现了 WASM 模块的加载和验证框架，
// 实际的 WASM 虚拟机集成（如 WAMR、Wasmtime）将在后续阶段通过
// 构建依赖添加。当前代码提供了清晰的接口和扩展点。
//
// 两个组件：
// ==========
// 1. WasmRuntime 单例
//    - Init():            初始化 WASM 运行时
//    - LoadModule(name, path): 加载 .wasm 文件模块
//    - GetModule(name):   获取已加载的模块字节码
//    - 验证模块文件魔法数字（0x00 0x61 0x73 0x6D = "\0asm"）
//
// 2. WasmProcessorPlugin
//    - 一个 ProcessorPlugin 的 WASM 适配器
//    - Process() 当前为透传（pass-through）
//    - 完整实现将：序列化输入 → 调用 WASM 函数 → 反序列化输出
//
// 安全设计：
// ==========
// WASM 沙箱天然提供内存隔离和权限控制：
// - 插件只能访问显式传递给它的内存
// - 无法调用任意系统调用
// - 无法访问宿主进程的文件系统和网络
// - 资源消耗可以限制（最大内存、最大执行指令数）
// ============================================================================

#pragma once

#include <cstdint>       // uint8_t 等固定宽度整数
#include <fstream>        // 读取 .wasm 文件
#include <string>
#include <vector>
#include <unordered_map>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/common/data_batch.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ============================================================================
// WasmRuntime — WASM 模块加载和运行环境
// ============================================================================
class WasmRuntime {
public:
    // 单例访问
    static WasmRuntime& Instance() {
        static WasmRuntime inst;
        return inst;
    }

    // 初始化 WASM 运行时环境。当前为 stub 实现。
    Status Init() {
        IL_INFO("WASM runtime initialized (stub)");
        initialized_ = true;
        return Status::Ok();
    }

    // 判断 WASM 运行时是否已就绪
    bool IsAvailable() const { return initialized_; }

    // ---- 加载 WASM 模块 ----
    // name: 模块的逻辑名称（用于后续引用）
    // path: .wasm 文件的路径
    // 成功返回 Ok()，失败返回错误状态
    Status LoadModule(const std::string& name, const std::string& path) {
        // 以二进制方式读取整个 .wasm 文件
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return Status::Error(StatusCode::kNotFound,
                "WASM module not found: " + path);
        }

        // 读取全部字节
        std::vector<uint8_t> bytes(
            std::istreambuf_iterator<char>(file), {});

        // 验证 WASM 魔数：前 4 字节必须是 0x00 0x61("a") 0x73("s") 0x6D("m")
        // 即 ASCII "\0asm"
        if (bytes.size() < 4 || bytes[0] != 0x00 || bytes[1] != 0x61 ||
            bytes[2] != 0x73 || bytes[3] != 0x6d) {
            return Status::Error(StatusCode::kInvalidArgument,
                "Not a valid WASM file: " + path);
        }

        // 存储模块字节码（后续由 WASM VM 解释执行）
        modules_[name] = std::move(bytes);
        IL_INFO("Loaded WASM module: {} ({} bytes)", name,
                modules_[name].size());
        return Status::Ok();
    }

    // ---- 获取已加载模块 ----
    // 返回模块字节码的指针，未找到返回 nullptr
    const std::vector<uint8_t>* GetModule(const std::string& name) const {
        auto it = modules_.find(name);
        return it != modules_.end() ? &it->second : nullptr;
    }

    // ---- 列出所有已加载模块名称 ----
    std::vector<std::string> ListModules() const {
        std::vector<std::string> names;
        for (auto& [k, _] : modules_) names.push_back(k);
        return names;
    }

private:
    WasmRuntime() = default;
    bool initialized_ = false;                                          // 是否已初始化
    std::unordered_map<std::string, std::vector<uint8_t>> modules_;    // 模块名称 → 字节码
};

// ============================================================================
// WasmProcessorPlugin — 基于 WASM 的数据处理器适配器
// ============================================================================
// 将 WASM 模块包装为标准的 ProcessorPlugin，使其可以加入任何 Pipeline。
// 当前为占位实现，Process() 将输入原样返回。
//
// 完整实现的数据流：
//   DataBatch(输入) → JSON 序列化 → WASM process() → JSON 反序列化 → DataBatch(输出)
class WasmProcessorPlugin : public ProcessorPlugin {
public:
    // 构造时指定要使用的 WASM 模块名称
    explicit WasmProcessorPlugin(const std::string& module_name)
        : module_name_(module_name) {}

    const char* Name() const override { return module_name_.c_str(); }
    const char* Version() const override { return "wasm-0.1.0"; }

    // 初始化：验证 WASM 运行时已启动、目标模块已加载
    Status Init(const ConfigValue& config) override {
        if (!WasmRuntime::Instance().IsAvailable()) {
            return Status::Error(StatusCode::kUnavailable,
                "WASM runtime not initialized");
        }
        auto* mod = WasmRuntime::Instance().GetModule(module_name_);
        if (!mod) {
            return Status::Error(StatusCode::kNotFound,
                "WASM module not loaded: " + module_name_);
        }
        IL_INFO("WASM processor '{}' initialized", module_name_);
        return Status::Ok();
    }

    // 数据处理（当前透传，完整实现如下）：
    // 1. 将输入的 DataBatch 序列化为 WASM 可读格式（如 JSON）
    // 2. 调用 WASM 模块的 process() 导出函数
    // 3. 将 WASM 输出反序列化回 DataBatch
    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        // TODO: 完整 WASM VM 集成时将实现序列化/调用/反序列化
        return input;  // 当前：原样透传
    }

private:
    std::string module_name_;  // 关联的 WASM 模块名称
};

}  // namespace illuminator
