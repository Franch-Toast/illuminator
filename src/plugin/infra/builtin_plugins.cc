// ============================================================================
// Illuminator 内置插件强链接清单
// ============================================================================
//
// 本文件通过 #include 引用所有内置插件的头文件，确保它们的
// 静态初始化器（IL_REGISTER_* 宏）被链接到最终二进制中。
//
// 当前所有具体插件已清除，等待按照新的架构规范重新实现。
// 添加新插件时，在此文件中 #include 其头文件即可。
// ============================================================================

#include "plugin/infra/builtin_plugins.h"

// ---- 数据源 (Source) ----
// 添加新 Source 插件的 #include 到这里

// ---- 数据处理器 (Processor) ----
// 添加新 Processor 插件的 #include 到这里

// ---- 聚合器 (Aggregator) ----
// 添加新 Aggregator 插件的 #include 到这里

// ---- 数据出口 (Sink) ----
// 添加新 Sink 插件的 #include 到这里

// ---- 存储后端 ----
// SQLiteBackend 通过 alwayslink=True 在 server 层级链接，不再在此处 include

namespace illuminator {

void RegisterBuiltinPlugins() {
    // 静态注册通过 IL_REGISTER_* 宏自动完成，函数体为空。
}

}  // namespace illuminator
