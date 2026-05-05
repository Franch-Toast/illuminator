// ============================================================================
// SchedAnalyzerSource — eBPF 调度分析器（调度延迟与迁移追踪）
// ============================================================================
//
// 通过 eBPF 挂载到内核调度器关键事件点（进程切换、唤醒、迁移），
// 实时采集进程调度延迟、运行队列等待时间、CPU 核心间迁移频次等指标。
// 用于诊断 CPU 调度瓶颈、NUMA 亲和性问题、调度延迟过高等场景。
//
// 采集指标：
// ==========
// 在聚合模式下，从 sched_agg map 中每次 Collect 取出各进程的累计统计：
//   - switch_count：上下文切换次数（被调度上/下 CPU 的次数）
//   - total_runqueue_latency_ns：运行队列中累计等待时间（纳秒）
//   - max_runqueue_latency_ns：运行队列最大单次等待时间（纳秒）
//   - migrate_count：CPU 核心迁移次数
//
// 在详细模式（detailed_mode）下，通过 ring buffer 实时推送每条调度事件：
//   - 事件类型：switch（上下文切换）、wakeup（进程唤醒）、migrate（核心迁移）
//   - prev_pid / next_pid：切换前后进程 PID
//   - prev_comm / next_comm：切换前后进程名
//   - cpu：事件发生的 CPU 核心
//   - latency_ns：延迟时间（纳秒）
//
// 工作原理：
// ==========
// 1. 通过 eBPF 挂载到以下内核 tracepoint：
//    - sched/sched_switch：进程切换事件（捕获 prev → next 的任务切换，计算运行队列延迟）
//    - sched/sched_wakeup / sched/sched_wakeup_new：进程唤醒事件（记录被唤醒进程的等待时间）
//    - sched/sched_migrate_task（可选）：进程迁移事件（记录进程从哪个核迁移到哪个核）
//
// 2. 两种工作模式：
//    - 聚合模式（默认，detailed_mode=false）：
//      每个 eBPF 事件触发时，更新 sched_agg map 中的 per-PID 聚合统计。
//      Collect() 方法遍历 map 中所有条目，取出统计值后删除（reset）。
//
//    - 详细模式（detailed_mode=true）：
//      通过 sched_analyzer_events ring buffer 推送每条原始调度事件。
//      按事件类型分类（switch/wakeup/migrate），适合做时序追踪或分布分析。
//
// 配置参数：
// ==========
// - detailed_mode：是否启用详细事件推送（默认 false）
// - aggregate_interval_ms：聚合统计输出间隔（默认 5000ms）
// - track_migrations：是否追踪 CPU 核心迁移事件（默认 true）
// - target_pids：逗号分隔的 PID 白名单（空 = 追踪所有进程）
// - bpf_object：eBPF 目标文件路径（必需）
// ============================================================================

#pragma once

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <bpf/libbpf.h>

#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/bpf_program_manager.h"
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ============================================================================
// ParseCommaSeparatedUint32sLocal — 解析逗号分隔的 uint32 列表（本地版）
// ============================================================================
// 功能与 cpu_profiler.h 中的 ParseCommaSeparatedInts 相同，
// 独立实现以避免跨源文件依赖。
inline void ParseCommaSeparatedUint32sLocal(const std::string& s,
                                            std::vector<uint32_t>* out) {
    out->clear();
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.pop_back();
        if (token.empty())
            continue;
        try {
            out->push_back(static_cast<uint32_t>(std::stoul(token)));
        } catch (...) {}
    }
}

// ============================================================================
// SchedAnalyzerSource 类 — 调度分析插件主体
// ============================================================================
class SchedAnalyzerSource : public SourcePlugin {
public:
    const char* Name() const override { return "sched_analyzer"; }
    const char* Version() const override { return "0.2.0"; }

    // 详细模式下为 Push 模式（ring buffer 主动推送事件）
    bool IsPushMode() const override { return detailed_mode_; }

    uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

    // ========================================================================
    // Init — 初始化插件配置
    // ========================================================================
    Status Init(const ConfigValue& config) override {
        detailed_mode_ = config["detailed_mode"].AsBool(false);
        aggregate_interval_ms_ =
            static_cast<uint32_t>(config["aggregate_interval_ms"].AsInt(5000));
        track_migrations_ = config["track_migrations"].AsBool(true);
        bpf_obj_path_ = config["bpf_object"].AsString("");
        ParseCommaSeparatedUint32sLocal(config["target_pids"].AsString(""),
                                      &target_pids_);
        // 构建 PID 快速查找集合
        target_pid_allow_.clear();
        for (uint32_t p : target_pids_)
            target_pid_allow_.insert(p);
        return Status::Ok();
    }

    // ========================================================================
    // Start — 加载 eBPF 程序并挂载调度 tracepoint
    // ========================================================================
    // 1. 加载 eBPF 目标文件
    // 2. 设置 sched_analyzer_cfg 配置 map（模式标志、迁移追踪标志）
    // 3. 挂载 eBPF 程序到 sched/sched_wakeup、sched/sched_switch、
    //    （可选）sched/sched_migrate_task 等内核 tracepoint
    // 4. 详细模式下创建 ring buffer 和后台轮询线程
    Status Start() override {
        if (bpf_obj_path_.empty()) {
            IL_WARN(
                "sched_analyzer: no bpf_object path specified; analyzer idle");
            running_.store(false);
            return Status::Ok();
        }

        // 加载 eBPF 对象
        auto st = bpf_mgr_.LoadObject("sched_analyzer", bpf_obj_path_);
        if (!st.ok())
            return st;

        // 写入配置标志位到 eBPF 侧
        // 位 0: detailed_mode, 位 1: track_migrations
        uint32_t cfg_flags =
            (detailed_mode_ ? 1u : 0u) | (track_migrations_ ? 2u : 0u);
        int cfg_fd = bpf_mgr_.GetMapFd("sched_analyzer", "sched_analyzer_cfg");
        if (cfg_fd >= 0) {
            uint32_t k = 0;
            bpf_map_update_elem(cfg_fd, &k, &cfg_flags, BPF_ANY);
        }

        // 挂载 eBPF 程序到内核调度事件点
        std::vector<std::string> progs = {"sched_analyzer_wakeup",
                                          "sched_analyzer_switch"};
        if (track_migrations_)
            progs.push_back("sched_analyzer_migrate");

        st = bpf_mgr_.AttachPrograms("sched_analyzer", progs);
        if (!st.ok())
            return st;

        // 获取聚合 map 的文件描述符
        agg_fd_ = bpf_mgr_.GetMapFd("sched_analyzer", "sched_agg");

        // 详细模式：创建 ring buffer 和后台轮询线程
        if (detailed_mode_) {
            int rb_fd =
                bpf_mgr_.GetMapFd("sched_analyzer", "sched_analyzer_events");
            if (rb_fd < 0) {
                return Status::Error(StatusCode::kInternal,
                                     "sched_analyzer_events ringbuf missing");
            }
            ring_buf_ =
                bpf_mgr_.CreateRingBuffer(rb_fd, HandleDetailedEvent, this);
            if (!ring_buf_) {
                return Status::Error(StatusCode::kInternal,
                                     "sched_analyzer ring buffer failed");
            }
            running_.store(true);
            poll_thread_ = std::thread([this] {
                while (running_.load())
                    ring_buffer__poll(ring_buf_, 100);
            });
        } else {
            running_.store(true);
        }

        IL_INFO("sched_analyzer started (detailed=%d migrations=%d)",
                detailed_mode_, track_migrations_);
        return Status::Ok();
    }

    // ========================================================================
    // Stop — 停止调度分析，清理资源
    // ========================================================================
    Status Stop() override {
        running_.store(false);
        if (poll_thread_.joinable())
            poll_thread_.join();
        if (ring_buf_) {
            ring_buffer__free(ring_buf_);
            ring_buf_ = nullptr;
        }
        bpf_mgr_.DetachAll();
        agg_fd_ = -1;
        return Status::Ok();
    }

    // ========================================================================
    // Collect — 聚合模式下的轮询采集入口
    // ========================================================================
    // 遍历 sched_agg map 中的所有 PID 条目，读取其调度统计后删除。
    // 对于不在目标 PID 白名单中的条目也执行删除（清理不需要的数据）。
    // 详细模式下 Collect 返回空 batch（数据已通过 ring buffer 推送）。
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        if (detailed_mode_ || agg_fd_ < 0)
            return batch;

        // 先收集所有 PID key（遍历时不能修改 map）
        uint32_t cur{}, next{};
        std::vector<uint32_t> pids;
        int err = bpf_map_get_next_key(agg_fd_, nullptr, &cur);
        while (err == 0) {
            pids.push_back(cur);
            err = bpf_map_get_next_key(agg_fd_, &cur, &next);
            cur = next;
        }

        for (uint32_t pid : pids) {
            il_sched_stats stats{};
            if (bpf_map_lookup_elem(agg_fd_, &pid, &stats) != 0)
                continue;

            // 不在白名单的 PID 也删除其数据（避免 map 膨胀）
            if (!AllowPid(pid)) {
                bpf_map_delete_elem(agg_fd_, &pid);
                continue;
            }

            bpf_map_delete_elem(agg_fd_, &pid);

            auto& rec = batch->AddRecord();
            rec.labels.push_back({batch->InternString("type"),
                                  batch->InternString("sched")});
            rec.labels.push_back({batch->InternString("mode"),
                                  batch->InternString("aggregated")});

            std::string comm(stats.comm,
                              strnlen(stats.comm, TASK_COMM_LEN));
            if (!comm.empty()) {
                rec.labels.push_back({batch->InternString("comm"),
                                      batch->InternString(comm)});
            }

            rec.SetField(batch->InternString("pid"), static_cast<uint64_t>(pid));
            rec.SetField(batch->InternString("switch_count"),
                         stats.switch_count);
            rec.SetField(batch->InternString("total_runqueue_latency_ns"),
                         stats.total_runqueue_latency_ns);
            rec.SetField(batch->InternString("max_runqueue_latency_ns"),
                         stats.max_runqueue_latency_ns);
            rec.SetField(batch->InternString("migrate_count"),
                         stats.migrate_count);
        }

        return batch;
    }

private:
    // ========================================================================
    // AllowPid — 检查 PID 是否在白名单中
    // ========================================================================
    // 如果白名单为空，允许所有 PID；否则只允许在白名单中的 PID。
    bool AllowPid(uint32_t pid) const {
        if (target_pid_allow_.empty())
            return true;
        return target_pid_allow_.find(pid) != target_pid_allow_.end();
    }

    // ========================================================================
    // HandleDetailedEvent — 详细模式的事件回调
    // ========================================================================
    // 当 ring buffer 收到一条 sched_event 时被调用。
    // 根据事件类型（switch/wakeup/migrate）分类输出，包含参与双方的
    // PID/进程名、CPU 核心、延迟时间等完整信息。
    static int HandleDetailedEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<SchedAnalyzerSource*>(ctx);
        if (size < sizeof(il_sched_event))
            return 0;
        auto* event = static_cast<il_sched_event*>(data);

        // 双方 PID 都必须通过白名单检查
        if (!self->AllowPid(event->prev_pid) &&
            !self->AllowPid(event->next_pid))
            return 0;

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kTrace);
        auto& rec = batch->AddRecord();

        const char* evt_types[] = {"switch", "wakeup", "migrate"};
        unsigned idx =
            event->event_type <
                    sizeof(evt_types) / sizeof(evt_types[0])
                ? event->event_type
                : 0;

        rec.labels.push_back({batch->InternString("type"),
                              batch->InternString("sched")});
        rec.labels.push_back({batch->InternString("event"),
                              batch->InternString(evt_types[idx])});

        rec.SetField(batch->InternString("prev_pid"),
                     static_cast<uint64_t>(event->prev_pid));
        rec.SetField(batch->InternString("next_pid"),
                     static_cast<uint64_t>(event->next_pid));
        rec.SetField(batch->InternString("cpu"),
                     static_cast<uint64_t>(event->cpu));
        rec.SetField(batch->InternString("latency_ns"),
                     static_cast<uint64_t>(event->latency_ns));
        rec.SetField(batch->InternString("prev_comm"),
                     batch->InternString(std::string_view(
                         event->prev_comm,
                         strnlen(event->prev_comm, TASK_COMM_LEN))));
        rec.SetField(batch->InternString("next_comm"),
                     batch->InternString(std::string_view(
                         event->next_comm,
                         strnlen(event->next_comm, TASK_COMM_LEN))));

        // 通过 callback_ 立即推送到下游管道
        if (self->callback_)
            self->callback_(std::move(batch));
        return 0;
    }

    // ---- 配置参数 ----
    bool detailed_mode_ = false;              // 是否启用详细事件推送
    uint32_t aggregate_interval_ms_ = 5000;   // 聚合统计输出间隔（毫秒）
    bool track_migrations_ = true;            // 是否追踪 CPU 迁移事件
    std::string bpf_obj_path_;                // eBPF 目标文件路径
    std::vector<uint32_t> target_pids_;       // 目标 PID 列表
    std::unordered_set<uint32_t> target_pid_allow_;  // PID 快速查找集合

    // ---- 运行时状态 ----
    std::atomic<bool> running_{false};        // 运行中标志
    BpfProgramManager bpf_mgr_;               // eBPF 程序管理器
    struct ring_buffer* ring_buf_ = nullptr;  // BPF ring buffer 句柄
    std::thread poll_thread_;                 // 详细模式的后台轮询线程
    int agg_fd_ = -1;                         // 聚合 map 的文件描述符
};

IL_REGISTER_SOURCE("sched_analyzer", SchedAnalyzerSource);

}  // namespace illuminator
