// =============================================================================
// 文件：filter_processor.h
// 模块：Illuminator 处理器 - 过滤处理器
// 描述：
//   根据指定的标签（label）键值对过滤数据记录。
//   当记录的标签集合中包含与配置匹配的排除规则时，该记录将被丢弃。
//   堆栈采样数据不受过滤影响，直接透传。
// =============================================================================

#pragma once

#include <string>
#include "plugin/api/processor_plugin.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

// FilterProcessor: 过滤处理器
// 基于标签匹配规则对 Record 进行过滤。
// 配置参数:
//   exclude_label_key   - 要匹配的标签键名
//   exclude_label_value - 要匹配的标签值（与键配合使用）
// 行为:
//   - 若 exclude_key_ 为空，则跳过所有过滤逻辑，直接返回输入
//   - 遍历每条 Record，检查其 labels 中是否存在匹配的键值对
//   - 命中的 Record 被丢弃，未命中的保留到输出中
//   - 所有 StackSample 直接复制到输出中
class FilterProcessor : public ProcessorPlugin {
public:
    // 返回处理器名称
    const char* Name() const override { return "filter"; }

    // 返回处理器版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析过滤规则
    // 参数:
    //   config - 配置项，应包含 exclude_label_key 和 exclude_label_value
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        auto key_sv = config["exclude_label_key"].AsString();
        auto val_sv = config["exclude_label_value"].AsString();
        exclude_key_ = std::string(key_sv);
        exclude_value_ = std::string(val_sv);
        return Status::Ok();
    }

    // 执行过滤处理
    // 参数:
    //   input - 输入的 DataBatch
    // 返回:
    //   StatusOr<DataBatchPtr> - 过滤后的 DataBatch，未被排除的 Record 和全部 StackSample
    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        // 未配置排除键则直接返回原始输入
        if (exclude_key_.empty()) return input;

        auto output = std::make_shared<DataBatch>(input->type());

        // 遍历所有 Record，将不满足排除条件的复制到输出
        for (auto& rec : input->records()) {
            bool excluded = false;
            for (auto& label : rec.labels) {
                if (label.key == exclude_key_ && label.value == exclude_value_) {
                    excluded = true;
                    break;
                }
            }
            if (!excluded) {
                output->records().push_back(rec);
            }
        }

        // 堆栈采样数据不受过滤影响，全部复制
        for (auto& sample : input->stack_samples()) {
            output->stack_samples().push_back(sample);
        }

        return output;
    }

private:
    // 要排除的标签键名
    std::string exclude_key_;

    // 要排除的标签值
    std::string exclude_value_;
};

// 在插件注册表中注册该处理器
IL_REGISTER_PROCESSOR("filter", FilterProcessor);

}  // namespace illuminator
