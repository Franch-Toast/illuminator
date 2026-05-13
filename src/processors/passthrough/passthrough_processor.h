// =============================================================================
// 文件：passthrough_processor.h
// 模块：Illuminator 处理器 - 透传处理器
// 描述：
//   提供最简单的"原样透传"处理器，不做任何数据变换。
//   主要用于流水线测试与调试场景，或作为默认占位处理器。
//   实现了 ProcessorPlugin 接口，通过 IL_REGISTER_PROCESSOR 自动注册。
// =============================================================================

#pragma once

#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// PassthroughProcessor: 透传处理器
// 完全不做任何处理，直接将输入 DataBatch 作为输出返回。
// 典型用途：
//   - 在未确定处理逻辑时作为占位节点
//   - 测试 PipelineController 框架是否正常串联
//   - 性能基准测试（测量框架本身开销）
class PassthroughProcessor : public ProcessorPlugin {
public:
    // 返回处理器名称，用于配置文件中按名称引用该处理器
    const char* Name() const override { return "passthrough"; }

    // 返回处理器版本号
    const char* Version() const override { return "0.1.0"; }

    // 核心处理逻辑：原样返回输入，不做任何修改
    // 参数:
    //   input - 输入的 DataBatch 智能指针
    // 返回:
    //   StatusOr<DataBatchPtr> - 直接返回原始 input，若输入为空则返回空指针
    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        return input;
    }
};

// 在插件注册表中注册该处理器，使其可通过配置文件中的 "passthrough" 标识符找到
IL_REGISTER_PROCESSOR("passthrough", PassthroughProcessor);

}  // namespace illuminator
