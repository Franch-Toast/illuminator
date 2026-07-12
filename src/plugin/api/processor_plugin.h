// ============================================================================
// Illuminator ProcessorPlugin — 数据处理器插件抽象
// ============================================================================
//
// Processor（处理器）是流水线中的数据转换环节，在 Source 和 Sink 之间
// 对数据进行实时加工。
//
// 一个 Pipeline 可以串联多个 Processor，数据依次经过每个处理器的 Process()
// 方法。每个处理器可以：
//   - 修改数据内容（如符号化堆栈地址、添加派生字段）
//   - 过滤数据（丢弃不符合条件的记录）
//   - 生成新数据（如展开、聚合、计算差值）
//   - 返回错误（停止处理该批次数据）
//
// 典型处理器示例：
// =================
// - PassthroughProcessor   — 透传（不做任何处理，用于测试）
// - FilterProcessor        — 按标签过滤记录
// - StackSymbolizerProcessor — 将堆栈地址解析为函数名
// - StackMergerProcessor   — 合并相同的调用栈
// ============================================================================

#pragma once

#include "plugin/api/plugin_api.h"

namespace illuminator {

class ProcessorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kProcessor; }

    // ---- 数据处理 ----
    // 接收一个 DataBatch shared_ptr，处理后返回处理结果。
    // - 可以原地修改后返回同一个 shared_ptr（最优性能）
    // - 可以创建新的 DataBatch 返回
    // - 返回 nullptr 表示丢弃整个批次
    // - 返回 error Status 表示处理失败
    virtual StatusOr<DataBatchPtr> Process(DataBatchPtr input) = 0;

    // 运行时动态修改处理参数（如过滤规则、符号化开关）。
    // 默认返回 Ok()（空操作），子类按需覆写。
    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Ok();
    }
};

}  // namespace illuminator
