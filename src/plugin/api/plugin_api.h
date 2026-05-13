// ============================================================================
// Illuminator 插件基础 API — 插件类型、基类、C ABI 接口
// ============================================================================
//
// 本文件是 Illuminator 插件系统的核心定义，包含：
// 1. PluginType 枚举 — 定义了四种插件类型
// 2. Plugin 基类 — 所有插件的抽象基类，定义生命周期接口
// 3. C ABI 接口 — 供 .so 动态库插件使用的 C 语言接口
//
// 插件生态的三个层次：
// ======================
// 1. 内置插件（Builtin）：编译时静态链接，通过 IL_REGISTER_* 宏注册
//    → 性能最优，零额外开销，适合核心功能的插件
//
// 2. 共享库插件（.so）：运行时通过 dlopen 加载的动态库
//    → 通过 C ABI 接口（IlPluginDescriptor）与主程序交互
//    → 用户可自行开发和替换，无需重新编译主程序
//
// 3. WASM 插件：通过 WebAssembly 沙箱加载的插件
//    → 安全隔离，适合不可信第三方的插件
//    → 当前为预留框架（stub），完整实现需要集成 WASM 运行时
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>

#include "core/common/config.h"
#include "core/common/status.h"
#include "core/engine/data_batch.h"

namespace illuminator {

// ---- 插件类型枚举 ----
// 每种类型对应流水线中的一个不同阶段
enum class PluginType {
    kSource,       // 数据源：负责产生原始观测数据
    kProcessor,    // 处理器：对数据进行实时转换
    kAggregator,   // 聚合器：在时间窗口内缓冲和聚合数据
    kSink,         // 数据出口：将数据写入目标位置
};

// 插件类型转字符串，用于日志和调试输出
inline const char* PluginTypeToString(PluginType type) {
    switch (type) {
        case PluginType::kSource:     return "source";
        case PluginType::kProcessor:  return "processor";
        case PluginType::kAggregator: return "aggregator";
        case PluginType::kSink:       return "sink";
    }
    return "unknown";
}

// ---- Plugin 抽象基类 ----
// 所有插件的公共接口，定义了插件的基本属性和生命周期。
// 各阶段插件（Source/Processor/Aggregator/Sink）在此基础上扩展专属方法。
//
// 生命周期：
//   构造 → Init(config) → Start() → [运行] → Stop() → 析构
//
// 其中 Init/Start/Stop 都返回 Status，便于统一错误处理。
// 默认实现返回 Ok()，插件可按需覆写。
class Plugin {
public:
    virtual ~Plugin() = default;

    // ---- 插件标识 ----
    // 名称（如 "cpu_utilization"、"stack_symbolizer"）
    virtual const char* Name() const = 0;
    // 版本号
    virtual const char* Version() const = 0;
    // 插件类型
    virtual PluginType Type() const = 0;

    // ---- 生命周期方法 ----

    // 初始化 — 传入配置参数，完成插件内部状态设置
    virtual Status Init(const ConfigValue& config) { return Status::Ok(); }
    // 启动 — 开始工作（如打开文件、启动线程、挂载 eBPF 探针）
    virtual Status Start() { return Status::Ok(); }
    // 停止 — 停止工作并释放资源
    virtual Status Stop() { return Status::Ok(); }
};

}  // namespace illuminator

// ============================================================================
// 稳定的 C ABI — 供共享库（.so）插件使用
// ============================================================================
// 使用 extern "C" 确保 C++ 编译器不会对函数签名进行名称修饰（Name Mangling），
// 使得 dlopen/dlsym 能够正确找到导出符号。
//
// 每个 .so 插件必须导出一个名为 "illuminator_plugin_describe" 的函数，
// 返回一个指向 IlPluginDescriptor 结构体的指针，描述插件的基本信息。
extern "C" {

// 插件描述符结构体
struct IlPluginDescriptor {
    const char* name;       // 插件名称
    const char* version;    // 插件版本
    uint32_t type;    // 插件类型（illuminator::PluginType 转为 uint32_t）
    void* (*create)(const char* config_json);  // 创建插件实例的工厂函数
    int   (*process)(void* ctx, void* input, void* output);  // 数据处理函数
    void  (*destroy)(void* ctx);              // 销毁插件实例的析构函数
};

// 导出函数类型：每个 .so 必须提供此签名的函数
typedef const IlPluginDescriptor* (*IlPluginDescribeFn)();

// 导出符号名称常量，dlsym 搜索时的键
#define IL_PLUGIN_EXPORT_NAME "illuminator_plugin_describe"

}  // extern "C"
