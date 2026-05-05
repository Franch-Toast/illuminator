// ============================================================================
// Illuminator AggregatorPlugin — 聚合器插件抽象
// ============================================================================
//
// Aggregator（聚合器）是可选的数据缓冲和聚合环节。它在 Processor 链之后、
// Sink 之前，定期将缓冲的时间窗口数据聚合并输出。
//
// 设计目的：
// ==========
// 在实时采集和最终输出之间插入一个缓冲层，实现：
// 1. 时间窗口聚合 — 计算 avg/min/max/P50/P99 等统计量
// 2. 数据压缩 — 将高频采样数据合并为低频统计结果，减少 Sink 压力
// 3. 堆栈合并 — 将相同调用栈的多次采样累积计数
// 4. 平滑输出 — 避免 Sink 被突发流量冲垮
//
// 与 Processor 的区别：
// ======================
// - Processor 对每个批次即时处理
// - Aggregator 在时间窗口内缓冲多个批次，然后在 Flush 时批量输出
// ============================================================================

#pragma once

#include <vector>
#include "plugin/api/plugin_api.h"

namespace illuminator {

class AggregatorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kAggregator; }

    // ---- 数据输入 ----
    // 将一批数据添加到聚合缓冲区
    virtual Status Add(DataBatchPtr batch) = 0;

    // ---- 数据输出 ----
    // 刷出聚合后的数据。由 Pipeline 的 flush 线程定期调用，
    // 也在管道停止时调用一次以输出最后的缓冲数据。
    // 返回零个或多个聚合后的 DataBatch。
    virtual StatusOr<std::vector<DataBatchPtr>> Flush() = 0;

    // ---- 刷新间隔 ----
    // 毫秒单位，默认 10 秒刷一次。设为 0 表示每来一批刷一批
    // （行为类似于无缓冲的 Processor）
    virtual uint32_t FlushIntervalMs() const { return 10000; }
};

}  // namespace illuminator
