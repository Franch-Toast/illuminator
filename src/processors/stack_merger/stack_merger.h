// =============================================================================
// 文件：stack_merger.h
// 模块：Illuminator 处理器 - 堆栈合并处理器
// 描述：
//   将多个重复的堆栈采样合并为单一采样，累加 count 值。
//   支持多种分组维度：按进程名 (comm)、进程 ID (pid)、线程 ID (tid)、全局。
//   可配置是否在合并键中包含内核栈信息。
// =============================================================================

#pragma once

#include <sstream>
#include <string>
#include <unordered_map>

#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// StackMergeGroupBy: 堆栈合并的分组粒度
// kComm   - 按进程名 (comm) 合并（默认）
// kPid    - 按进程 ID 合并
// kTid    - 按线程 ID 合并
// kGlobal - 全局合并（忽略所有进程/线程差异）
enum class StackMergeGroupBy {
    kComm,
    kPid,
    kTid,
    kGlobal,
};

// StackMergerProcessor: 堆栈合并处理器
// 将具有相同合并键（由 group_by 和堆栈内容决定）的多个 StackSample
// 合并为一条，count 字段累加。
// 这样可以减少冗余数据量，提高下游聚合和输出的效率。
// 配置参数:
//   group_by       - 合并分组维度: "comm" / "pid" / "tid" / "global"
//   include_kernel - 是否将内核栈纳入合并键，默认 true
class StackMergerProcessor : public ProcessorPlugin {
public:
    // 返回处理器名称
    const char* Name() const override { return "stack_merger"; }

    // 返回处理器版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析分组策略和内核栈包含选项
    // 参数:
    //   config - 配置项，包含 group_by 和 include_kernel
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        auto gb = config["group_by"].AsString("comm");
        if (gb == "pid")
            group_by_ = StackMergeGroupBy::kPid;
        else if (gb == "tid")
            group_by_ = StackMergeGroupBy::kTid;
        else if (gb == "global")
            group_by_ = StackMergeGroupBy::kGlobal;
        else
            group_by_ = StackMergeGroupBy::kComm;

        include_kernel_ = config["include_kernel"].AsBool(true);
        return Status::Ok();
    }

    // 执行堆栈合并处理
    // 参数:
    //   input - 包含堆栈采样数据的 DataBatch
    // 返回:
    //   StatusOr<DataBatchPtr> - 合并后的 DataBatch，其中重复堆栈已被合并
    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        if (!input || input->stack_samples().empty())
            return input;

        // 使用 unordered_map 按合并键去重
        std::unordered_map<std::string, StackSample> merged;
        merged.reserve(input->stack_samples().size());

        for (const auto& src : input->stack_samples()) {
            std::string key = StackKey(src);
            auto it = merged.find(key);
            if (it == merged.end())
                // 首次出现该键，直接插入
                merged.emplace(std::move(key), src);
            else
                // 已存在，累加 count
                it->second.count += src.count;
        }

        auto out = std::make_shared<DataBatch>(input->type());
        // 将合并结果写入输出批次
        for (auto& [_, sample] : merged) {
            auto& dst = out->AddStackSample();
            dst.timestamp = sample.timestamp;
            dst.pid = sample.pid;
            dst.tid = sample.tid;
            dst.comm = out->InternString(sample.comm);
            dst.count = sample.count;
            dst.cpu = sample.cpu;
            dst.sample_type = sample.sample_type;
            dst.duration_ns = sample.duration_ns;
            dst.kernel_stack_id = sample.kernel_stack_id;
            dst.user_stack_id = sample.user_stack_id;

            // 复制内核栈帧（使用 intern 字符串节省内存）
            dst.kernel_stack.reserve(sample.kernel_stack.size());
            for (const auto& fr : sample.kernel_stack) {
                StackFrame nf;
                nf.address = fr.address;
                nf.function_name = out->InternString(fr.function_name);
                nf.file_name = out->InternString(fr.file_name);
                nf.line_number = fr.line_number;
                nf.module_name = out->InternString(fr.module_name);
                dst.kernel_stack.push_back(nf);
            }

            // 复制用户栈帧（同样使用 intern 字符串）
            dst.user_stack.reserve(sample.user_stack.size());
            for (const auto& fr : sample.user_stack) {
                StackFrame nf;
                nf.address = fr.address;
                nf.function_name = out->InternString(fr.function_name);
                nf.file_name = out->InternString(fr.file_name);
                nf.line_number = fr.line_number;
                nf.module_name = out->InternString(fr.module_name);
                dst.user_stack.push_back(nf);
            }
        }

        // Record 数据不受合并影响，直接复制
        for (auto& rec : input->records())
            out->records().push_back(rec);

        return out;
    }

private:
    // 根据当前分组策略和堆栈内容生成唯一的合并键
    // 合并键格式取决于 group_by_ 和 include_kernel_:
    //   - 前缀: 分组标识（comm/pid:tid/global）
    //   - 内核栈: 以 ';' 分隔的函数名链（若 include_kernel_ 为 true）
    //   - 分隔符: '|'
    //   - 用户栈: 以 ';' 分隔的函数名链
    // 参数:
    //   s - 待生成键的栈采样
    // 返回:
    //   唯一的字符串合并键
    std::string StackKey(const StackSample& s) const {
        std::ostringstream os;
        switch (group_by_) {
            case StackMergeGroupBy::kComm:
                os << std::string(s.comm) << "|";
                break;
            case StackMergeGroupBy::kPid:
                os << s.pid << "|";
                break;
            case StackMergeGroupBy::kTid:
                os << s.pid << ":" << s.tid << "|";
                break;
            case StackMergeGroupBy::kGlobal:
                break;
        }

        // 可选：将内核栈纳入合并键
        if (include_kernel_) {
            for (const auto& fr : s.kernel_stack) {
                if (!fr.function_name.empty())
                    os << std::string(fr.function_name) << ";";
                else
                    os << "k:" << fr.address << ";";
            }
            os << "|";
        }

        // 用户栈始终纳入合并键
        for (const auto& fr : s.user_stack) {
            if (!fr.function_name.empty())
                os << std::string(fr.function_name) << ";";
            else
                os << "u:" << fr.address << ";";
        }
        return os.str();
    }

    // 当前合并分组粒度，默认按进程名 (comm)
    StackMergeGroupBy group_by_ = StackMergeGroupBy::kComm;

    // 是否将内核栈纳入合并键
    bool include_kernel_ = true;
};

// 在插件注册表中注册该处理器
IL_REGISTER_PROCESSOR("stack_merger", StackMergerProcessor);

}  // namespace illuminator
