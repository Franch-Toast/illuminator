// =============================================================================
// 文件：pprof_export_sink.h
// 模块：Illuminator 数据出口 - pprof 导出
// 描述：
//   将堆栈采样数据导出为 FlameGraph 兼容的折叠格式（folded format）。
//   支持通过 FlameGraph 工具链生成火焰图可视化。
//   完整 Protobuf pprof 格式需要 protobuf 依赖（暂未实现）。
// =============================================================================

#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

#include "plugin/api/sink_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// PprofExportSink: pprof/FlameGraph 导出 Sink
// 将 StackSample 数据转换为折叠栈格式（comm;func1;func2;...;funcN <count>）
// 并在 Flush() 时将累积结果写入文件，清空内部计数器。
// 兼容 FlameGraph、speedscope 等火焰图工具。
// 配置参数:
//   path - 输出文件路径，默认 /tmp/illuminator.folded
class PprofExportSink : public SinkPlugin {
public:
    // 返回 Sink 名称
    const char* Name() const override { return "pprof_export"; }

    // 返回 Sink 版本号
    const char* Version() const override { return "0.1.0"; }

    // 从配置中解析输出文件路径
    // 参数:
    //   config - 配置项，包含 path 字段
    // 返回:
    //   Status::Ok() 表示初始化成功
    Status Init(const ConfigValue& config) override {
        path_ = config["path"].AsString("/tmp/illuminator.folded");
        return Status::Ok();
    }

    // 将 StackSample 转换为折叠栈并累加 count
    // 使用 unordered_map 以栈内容为键累加采样次数。
    // 参数:
    //   batch - 包含 StackSample 的数据批次
    // 返回:
    //   Status::Ok() 表示成功
    Status Write(DataBatchPtr batch) override {
        if (!batch) return Status::Ok();

        for (auto& sample : batch->stack_samples()) {
            std::string key = BuildFoldedStack(sample);
            if (!key.empty()) {
                folded_counts_[key] += sample.count;
            }
        }
        return Status::Ok();
    }

    // 将累积的折叠栈数据写入文件并清空缓存
    // 写入格式: <折叠栈>\t<count>
    // 返回:
    //   Status::Ok() 表示成功；kInternal 表示文件无法打开
    Status Flush() override {
        if (folded_counts_.empty()) return Status::Ok();

        std::ofstream out(path_, std::ios::trunc);
        if (!out.is_open()) {
            return Status::Error(StatusCode::kInternal,
                "Cannot open pprof output: " + path_);
        }

        for (auto& [stack, count] : folded_counts_) {
            out << stack << " " << count << "\n";
        }

        out.close();
        IL_INFO("pprof export: {} unique stacks written to {}",
                folded_counts_.size(), path_);
        folded_counts_.clear();
        return Status::Ok();
    }

    // 停止 Sink 时执行最后一次 Flush
    // 返回:
    //   Status::Ok() 表示成功
    Status Stop() override {
        return Flush();
    }

private:
    // 将 StackSample 转换为 FlameGraph 折叠格式
    // 格式: comm;leaf_func;...;root_func
    // 用户栈从叶到根方向（rbegin -> rend），以 ';' 分隔。
    // 参数:
    //   sample - 待转换的堆栈采样
    // 返回:
    //   折叠栈字符串，如 "bash;main;0x1234"
    static std::string BuildFoldedStack(const StackSample& sample) {
        std::string result;
        result.reserve(512);

        // 先写进程名（comm）作为根节点
        if (!sample.comm.empty()) {
            result.append(sample.comm.data(), sample.comm.size());
        }

        // 从用户栈深层开始，逐帧拼接，构建 comm;frame1;frame2;...;frameN
        for (auto it = sample.user_stack.rbegin();
             it != sample.user_stack.rend(); ++it) {
            result += ";";
            if (!it->function_name.empty()) {
                result.append(it->function_name.data(), it->function_name.size());
            } else {
                // 函数名缺失时用地址代替
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%lx", it->address);
                result += buf;
            }
        }

        return result;
    }

    std::string path_;                                    // 输出文件路径
    std::unordered_map<std::string, uint64_t> folded_counts_; // 折叠栈 -> 累计 count
};

// 在插件注册表中注册该 Sink
IL_REGISTER_SINK("pprof_export", PprofExportSink);

}  // namespace illuminator
