// ============================================================================
// FeatureManager — 功能级生命周期管理（按需启停 + 实时流 + 录制）
// ============================================================================
//
// 【架构定位】
// FeatureManager 是引擎层的"用户界面"——它向上对接 REST API / WebSocket 层，
// 向下通过 PipelineController 共享基础设施（TimerWheel、CollectPool、SinkPool）。
// 它不直接管理线程池或定时器，而是通过 PipelineController 间接操作。
//
// 【核心概念】
//   Feature（功能） = 面向用户的最小可操作单元
//   1 Feature     = 1 Pipeline（数据处理管道）
//   1 Feature     = 1 前端面板（Web UI 中的一个监控/分析面板）
//
//   例如：
//     - "cpu_utilization"  Feature → 对应 Pipeline[cpu_utilization]
//     - "cpu_profiler"     Feature → 对应 Pipeline[cpu_profiler]
//     - "ebpf_io_monitor"  Feature → 对应 Pipeline[ebpf_io_monitor]
//
// 【与 PipelineController 的关系】
//   PipelineController 是"基础设施管理者"：
//     - 管理 TimerWheel（全局定时调度器）、CollectPool（采集线程池）、SinkPool（写入线程池）
//     - 从 YAML 配置构建所有 Pipeline
//     - 负责全量启动/停止
//
//   FeatureManager 是"功能调度者"：
//     - 按需启停单个 Feature（用户点击"开始采集"按钮时触发）
//     - 管理 Feature 的生命周期状态机
//     - 为每个 Feature 自动注入 StreamSink（实时数据缓冲）和 RecordingSink（录制）
//     - 提供运行时状态查询（ListFeatures）
//
//   两者共享基础设施：
//     FeatureManager 持有 PipelineController& 引用，通过它访问 TimerWheel、
//     CollectPool、SinkPool，但不会重复创建这些资源。
//
// 【Feature 状态模型】
//   Inactive  —— 未激活（初始状态，或停止后恢复到此状态）
//     ↓ Start()
//   Starting  —— 启动中（Pipeline 正在创建，避免重复启动的过渡状态）
//     ↓ 创建成功
//   Active    —— 运行中（Pipeline 正常采集并处理数据）
//     ↓ Pause()            ↑ Resume()
//   Paused    —— 暂停（Pipeline 线程仍在运行，但定时器已取消，不采集数据）
//      ↓ Stop()              ↓ Stop()
//   Stopping  —— 停止中（Pipeline 正在销毁，定时器已取消）
//      ↓ 销毁完成
//   Inactive
//
//   【Pause/Resume 的设计意图】
//   暂停时只取消 TimerWheel 定时器，不销毁 Pipeline 线程。
//   这样 Resume 时只需重新注册定时器，无需重建整个 Pipeline，实现毫秒级恢复。
//   适用于"切换标签页 → 暂停"和"切回标签页 → 恢复"的场景。
//
// 【Feature 资源消耗等级 (FeatureTier)】
//   用于前端自动启动策略和资源预算管理：
//   Tier 1 (Monitoring): 纯 procfs 读取，CPU < 0.5%，切换标签页时自动启动
//   Tier 2 (Tracing):    轻量 eBPF + procfs，CPU 1-3%，自动启动
//   Tier 3 (Profiling):  高频采样，CPU 3-10%，需手动触发（必须指定 target_pids）
//
// 【Tier 3 安全保护】
//   Profiling 类 Feature 如果不对采样范围做限制，会对整个系统所有进程进行
//   高频采样，可能导致系统负载爆增甚至死机。因此 FeatureManager 强制要求
//   Tier 3 Feature 必须指定 target_pids，否则 Start() 会返回错误。
//
// 【自动注入 Sink 机制】
//   每个 Feature 的 Pipeline 创建时，除了用户配置的 Sink（如 console_output、
//   file_export），FeatureManager 还会自动注入两个 Sink：
//
//   1. StreamSink：统一数据缓冲
//      - 内部维护一个环形缓冲区（Ring Buffer），存储最近的 N 条数据
//      - HTTP API (/api/v1/features/:name/collect) 从中读取最新数据快照
//      - WebSocketManager 直接从 StreamSinkStore 拉取数据广播给所有 WS 客户端
//      - 避免了为每个 Feature 单独创建 WebSocket 连接的复杂性
//
//   2. RecordingSink：按需录制
//      - 用户通过 API 触发录制（POST /api/v1/features/:name/record/start）
//      - 录制数据写入磁盘文件（.ilr 格式），可导出和回放
//      - 注册到全局 RecordingSinkRegistry，供 API 层查询录制状态
//
// 【线程安全模型】
//   - features_ map 使用 std::shared_mutex 保护
//   - 读操作（ListFeatures、GetState、GetPipeline）使用 shared_lock（允许多并发读）
//   - 写操作（Start、Stop、Pause、Resume、RegisterFeature）使用 unique_lock（互斥写）
//   - 状态变更回调（NotifyStateChange）在锁外调用，避免死锁
//
// 【数据流全景】
//   ┌─────────────────────────────────────────────────────────────────┐
//   │  REST API 层  │  WebSocket 层  │  前端 UI  │  录制/回放          │
//   └──────┬────────┴──────┬─────────┴───────────┴──────────┘         │
//          │               │                                           │
//   ┌──────▼───────────────▼──────────────────────────────────────┐   │
//   │                 FeatureManager                                │   │
//   │  ┌──────────────────────────────────────────────────────┐    │   │
//   │  │  features_ map:                                       │    │   │
//   │  │    "cpu_utilization" → FeatureEntry {                 │    │   │
//   │  │      config: {name, display_name, tier, pipeline...}  │    │   │
//   │  │      state: kActive                                   │    │   │
//   │  │      pipeline: Pipeline[cpu_utilization]              │    │   │
//   │  │      timer_ids: [collect_timer_id, flush_timer_id]    │    │   │
//   │  │    }                                                  │    │   │
//   │  │    "cpu_profiler" → FeatureEntry { ... }              │    │   │
//   │  │    ...                                                │    │   │
//   │  └──────────────────────────────────────────────────────┘    │   │
//   └──────────┬───────────────────────────────────────────────────┘   │
//              │ 共享基础设施引用                                       │
//   ┌──────────▼───────────────────────────────────────────────────┐   │
//   │              PipelineController                               │   │
//   │  ┌──────────┐  ┌──────────────┐  ┌──────────┐               │   │
//   │  │TimerWheel│  │ CollectPool  │  │ SinkPool │               │   │
//   │  │ (1线程)  │  │  (M 线程)    │  │ (K 线程) │               │   │
//   │  └──────────┘  └──────────────┘  └──────────┘               │   │
//   └──────────────────────────────────────────────────────────────┘   │
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/config.h"
#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/pipeline_controller.h"
#include "plugin/manager/plugin_registry.h"
#include "sinks/recording_sink/recording_sink.h"
#include "sinks/stream_sink/stream_sink.h"

namespace illuminator {

// ============================================================================
// FeatureState — Feature 生命周期状态机
// ============================================================================
//
// 状态转换路径（完整状态机）：
//   Inactive  ──Start()──→  Starting  ──创建成功──→  Active  ⇄  Paused
//                ↑              ↓                      ↓           ↓
//                │              └──创建失败──→          │           │
//                │                                  Stop()      Stop()
//                │                                    ↓           ↓
//                └──────────── 销毁完成 ──── Stopping ←───────────┘
//
// 说明：
//   - Starting 和 Stopping 是过渡状态，用于防止并发操作（例如用户在启动过程中
//     再次点击启动按钮，或者两个请求同时尝试停止同一个 Feature）
//   - Paused 状态保留 Pipeline 线程，只取消定时器，实现毫秒级恢复
//   - 只有在 Inactive 或 Paused 状态下才能调用 Start()
//   - 只有在 Active 或 Paused 状态下才能调用 Stop()
enum class FeatureState {
    kInactive,   // 未激活（初始状态，或停止后恢复到此状态）
    kStarting,   // 启动中（Pipeline 正在创建，避免重复启动）
    kActive,     // 运行中（Pipeline 正常采集并处理数据）
    kPaused,     // 暂停（Pipeline 线程仍在运行，但 TimerWheel 定时器已取消，不采集数据）
    kStopping,   // 停止中（Pipeline 正在销毁，TimerWheel 定时器已取消）
};

// 将 FeatureState 枚举转为字符串表示，用于日志输出和 API 响应
inline const char* FeatureStateToString(FeatureState s) {
    switch (s) {
        case FeatureState::kInactive:  return "inactive";
        case FeatureState::kStarting:  return "starting";
        case FeatureState::kActive:    return "active";
        case FeatureState::kPaused:    return "paused";
        case FeatureState::kStopping:  return "stopping";
    }
    return "unknown";
}

// ============================================================================
// FeatureTier — Feature 资源消耗等级
// ============================================================================
//
// 用于前端自动启动策略和资源预算管理。
//
// 等级划分依据：
//   Tier 1 (Monitoring): 纯 procfs 读取，CPU 开销 < 0.5%
//     例如：cpu_utilization（读取 /proc/stat）、process_cpu（读取 /proc/pid/stat）
//     策略：切换标签页时自动启动，用户无感知
//
//   Tier 2 (Tracing): 轻量 eBPF + procfs，CPU 开销 1-3%
//     例如：ebpf_io_monitor（IO 追踪）、ebpf_net_tracer（网络追踪）
//     策略：守护进程启动时自动启动，不阻塞 UI
//
//   Tier 3 (Profiling): 高频采样，CPU 开销 3-10%
//     例如：cpu_profiler（CPU 火焰图）、offcpu_profiler（Off-CPU 分析）
//     策略：必须手动触发，且必须指定 target_pids（防止系统级采样导致死机）
//
// 前端 Always-On 策略：
//   - Tier 1-2：守护进程启动时自动启动
//   - Tier 3：需要用户创建 Session 并指定目标进程后手动启动
enum class FeatureTier {
    kMonitoring = 1,  // Tier 1: 纯 procfs 读取，极低开销
    kTracing = 2,     // Tier 2: 轻量 eBPF + procfs，中等开销
    kProfiling = 3,   // Tier 3: 高频采样，高开销，需手动触发
};

// ============================================================================
// FeatureConfig — Feature 静态配置
// ============================================================================
//
// 注册时设定，运行时不会改变。相当于 Feature 的"出生证明"。
//
// 字段说明：
//   name:         内部标识名（如 "cpu_utilization"），用于 API 路由和内部查找
//   display_name: 前端显示名（如 "CPU 利用率"），用于 Web UI 面板标题
//   category:     分类（如 "cpu", "memory", "io", "network"），用于前端分组展示
//   tier:         资源消耗等级，决定 Always-On 策略和启动限制
//   pipeline:     对应的 Pipeline 配置，包含 Source/Processor/Aggregator/Sink 的配置
//
// 示例：
//   FeatureConfig cpu_config;
//   cpu_config.name = "cpu_utilization";
//   cpu_config.display_name = "CPU 利用率";
//   cpu_config.category = "cpu";
//   cpu_config.tier = FeatureTier::kMonitoring;
//   cpu_config.pipeline.source.type = "cpu_utilization";
//   cpu_config.pipeline.sinks = {{"console_output", {}}};
//   feature_manager.RegisterFeature(std::move(cpu_config));
struct FeatureConfig {
    std::string name;             // 内部标识名（如 "cpu_utilization"）
    std::string display_name;     // 前端显示名（如 "CPU 利用率"）
    std::string category;         // 分类（如 "cpu", "memory", "io"）
    FeatureTier tier = FeatureTier::kMonitoring;  // 资源消耗等级
    PipelineConfig pipeline;      // 对应的 Pipeline 配置（Source/Processor/Sink 链）
};

// ============================================================================
// FeatureInfo — Feature 运行时状态快照
// ============================================================================
//
// 由 ListFeatures() 方法返回，用于 REST API 响应和前端状态展示。
// 包含 Feature 的当前状态、运行统计和录制状态等运行时信息。
//
// 字段说明：
//   name/display_name/category/tier: 从 FeatureConfig 复制而来
//   state:              当前生命周期状态（Inactive/Active/Paused 等）
//   is_recording:       是否正在录制（通过 RecordingSinkRegistry 查询）
//   batches_processed:  已处理的数据批次数（从 Pipeline 统计中读取）
//   records_processed:  已处理的记录数（从 Pipeline 统计中读取）
//   errors:             错误计数（从 Pipeline 统计中读取）
//   uptime_ms:          运行时长（毫秒），只在 Active 或 Paused 状态下计算
//
// 注意：这些统计值都是 atomic 读取，与 Pipeline 的 ProcessThread 无锁共享。
struct FeatureInfo {
    std::string name;
    std::string display_name;
    std::string category;
    FeatureTier tier = FeatureTier::kMonitoring;
    FeatureState state;           // 当前生命周期状态
    bool is_recording = false;    // 是否正在录制
    uint64_t batches_processed = 0;  // 已处理批次数
    uint64_t records_processed = 0;  // 已处理记录数
    uint64_t errors = 0;             // 错误计数
    uint64_t uptime_ms = 0;          // 运行时长（毫秒）
};

// ============================================================================
// FeatureManager — Feature 生命周期管理器
// ============================================================================
//
// 【核心职责】
//   1. Feature 注册：将 YAML 配置中的 Pipeline 注册为 Feature
//   2. 按需启停：响应用户的启动/停止/暂停/恢复请求
//   3. 自动注入 Sink：为每个 Feature 的 Pipeline 自动添加 StreamSink 和 RecordingSink
//   4. 在线重配置：不重启 Pipeline 的情况下更新 Source 的过滤条件（例如切换目标进程）
//   5. 状态推送：通过回调通知 WebSocket 层 Feature 状态变化
//   6. 安全保护：Tier 3 Profiling Feature 必须指定 target_pids
//
// 【使用示例】
//   PipelineController controller;
//   FeatureManager fm(controller);
//
//   // 1. 注册 Feature
//   FeatureConfig cfg;
//   cfg.name = "cpu_utilization";
//   cfg.tier = FeatureTier::kMonitoring;
//   cfg.pipeline.source = {"cpu_utilization", {}};
//   fm.RegisterFeature(std::move(cfg));
//
//   // 2. 启动 Feature
//   fm.Start("cpu_utilization");
//
//   // 3. 查询状态
//   auto info = fm.ListFeatures();
//
//   // 4. 停止 Feature
//   fm.Stop("cpu_utilization");
//
// 【线程安全】
//   所有公开方法都是线程安全的。内部使用 shared_mutex 实现读多写少优化。
//   状态变更回调在锁外调用，避免回调中的死锁风险。
class FeatureManager {
public:
    // ========================================================================
    // 状态变更回调类型
    // ========================================================================
    // 当 Feature 状态变化时通知外部（如 WebSocketManager 推送状态更新到前端）。
    //
    // 参数：
    //   feature: Feature 名称
    //   from:    变更前的状态
    //   to:      变更后的状态
    //
    // 注意：回调在锁外调用，实现者不应在此回调中调用 FeatureManager 的
    // 可能获取锁的方法（如 Start/Stop），否则可能导致死锁。
    using StateChangeCallback = std::function<void(const std::string& feature,
                                                    FeatureState from,
                                                    FeatureState to)>;

    // ========================================================================
    // 构造
    // ========================================================================
    // 参数：
    //   controller: PipelineController 引用，FeatureManager 通过它访问
    //               TimerWheel、CollectPool、SinkPool 等共享基础设施。
    //               FeatureManager 不拥有 controller 的所有权，只是引用。
    //
    // 注意：controller 的生命周期必须长于 FeatureManager，否则会导致悬空引用。
    explicit FeatureManager(PipelineController& controller)
        : controller_(controller) {}

    // ========================================================================
    // RegisterFeature — 注册 Feature 配置（启动前调用）
    // ========================================================================
    //
    // 将 Feature 的静态配置注册到内部 features_ map 中，初始状态为 kInactive。
    //
    // 此方法只注册，不启动 Pipeline。启动需要单独调用 Start()。
    // 通常在守护进程初始化阶段，从 YAML 配置中读取所有 Pipeline 配置后批量注册。
    //
    // 如果同名的 Feature 已存在，会覆盖旧配置（因为 map 的 [] 操作符会覆盖）。
    //
    // 参数：
    //   config: Feature 的静态配置（移动语义，避免拷贝）
    void RegisterFeature(FeatureConfig config) {
        std::unique_lock lock(mutex_);
        auto& entry = features_[config.name];
        entry.config = std::move(config);
        entry.state = FeatureState::kInactive;
        IL_INFO("Feature registered: {}", entry.config.name);
    }

    // ========================================================================
    // StartParams — 启动参数
    // ========================================================================
    //
    // 运行时传入，可覆盖配置中的 target_pids 和 target_comms。
    //
    // 为什么需要运行时参数？
    //   配置文件中通常不指定 target_pids（因为进程 PID 在每次运行时都可能不同）。
    //   用户通过前端选择一个或多个进程后，前端通过 API 请求传入 target_pids，
    //   由 FeatureManager 将这些参数合并到 Source 的配置中。
    //
    // 字段说明：
    //   target_pids:  目标进程 PID 列表（例如 [1234, 5678]）
    //   target_comms: 目标进程名列表（例如 ["nginx", "mysqld"]）
    struct StartParams {
        std::vector<uint32_t> target_pids;      // 目标进程 PID 列表
        std::vector<std::string> target_comms;  // 目标进程名列表
    };

    // ========================================================================
    // Start — 启动 Feature
    // ========================================================================
    //
    // 状态转换：Inactive/Paused → Starting → Active
    //
    // 如果已 Active 且传入了新的 target_pids，则在线重配置过滤器（不重启 Pipeline）。
    // 这允许用户在不中断采集的情况下切换监控的目标进程。
    //
    // 安全保护：Tier 3 (Profiling) 必须指定 target_pids，防止系统级采样导致死机。
    // 检查逻辑：
    //   1. 检查 params.target_pids 是否为空
    //   2. 检查 pipeline.source.config["target_pids"] 是否为空
    //   3. 两者都为空时返回错误
    //
    // 启动流程：
    //   1. 参数校验（Feature 存在、状态合法、Tier 3 安全检查）
    //   2. 如果已 Active 且有新参数 → 调用 ReconfigureFilter 在线更新
    //   3. 如果 Paused → 调用 ResumeInternal 恢复
    //   4. 如果 Inactive → 创建并启动 Pipeline
    //      a. 通过 PluginRegistry 创建 Source/Processor/Sink 插件
    //      b. 自动注入 StreamSink（数据缓冲）和 RecordingSink（录制）
    //      c. 注册 Pull Source 采集定时器到 TimerWheel
    //      d. 注册 Aggregator 刷盘定时器到 TimerWheel
    //      e. 启动 Pipeline
    //
    // 参数：
    //   name:   Feature 名称
    //   params: 启动参数（可选，包含 target_pids 和 target_comms）
    //
    // 返回：
    //   Ok:     启动成功
    //   Error:  启动失败（原因见 message）
    Status Start(const std::string& name, const StartParams& params = {}) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Feature not found: " + name);
        }
        auto& entry = it->second;

        // 如果已经 Active 且有新的 target_pids，在线重配置（不重启）
        if (entry.state == FeatureState::kActive) {
            if (!params.target_pids.empty()) {
                lock.unlock();
                return ReconfigureFilter(name, params);
            }
            return Status::Ok();
        }

        // 状态合法性检查：只能在 Inactive 或 Paused 状态下启动
        if (entry.state != FeatureState::kInactive &&
            entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature cannot be started in state: " +
                                 std::string(FeatureStateToString(entry.state)));
        }

        // 如果已 Paused，直接恢复（不重建 Pipeline）
        if (entry.state == FeatureState::kPaused) {
            return ResumeInternal(entry);
        }

        // Tier 3 安全检查：Profiling 类 Feature 必须指定 target_pids
        // 防止对全系统进行高频采样，导致系统负载爆增甚至死机
        if (entry.config.tier == FeatureTier::kProfiling &&
            params.target_pids.empty() &&
            entry.config.pipeline.source.config["target_pids"].AsString("").empty()) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Profiling features require target_pids to prevent "
                                 "system-wide sampling (which may freeze the system). "
                                 "Pass target_pids in the request body.");
        }

        // 状态切换到 Starting（过渡状态，防止并发启动）
        auto prev = entry.state;
        entry.state = FeatureState::kStarting;
        lock.unlock();
        NotifyStateChange(name, prev, FeatureState::kStarting);

        // 创建并启动 Pipeline（锁外操作，避免长时间持锁）
        auto status = CreateAndStartPipeline(name, params);
        lock.lock();

        // 启动失败处理：回退到 Inactive 状态
        if (!status.ok()) {
            entry.state = FeatureState::kInactive;
            NotifyStateChange(name, FeatureState::kStarting, FeatureState::kInactive);
            return status;
        }

        // 启动成功：切换到 Active 状态，记录启动时间
        entry.state = FeatureState::kActive;
        entry.started_at = std::chrono::steady_clock::now();
        lock.unlock();
        NotifyStateChange(name, FeatureState::kStarting, FeatureState::kActive);
        IL_INFO("Feature started: {} (target_pids={})", name, params.target_pids.size());
        return Status::Ok();
    }

    // ========================================================================
    // ReconfigureFilter — 在线更新过滤器（不重启 Pipeline）
    // ========================================================================
    //
    // 更新 Source 的 target_pids / target_comms，并清除 StreamSinkStore 旧数据。
    //
    // 使用场景：
    //   用户在前端切换监控的目标进程时，不需要停止 Feature 再重新启动，
    //   直接通过此方法在线更新 Source 的过滤条件。
    //
    // 清除 StreamSinkStore 旧数据的原因：
    //   StreamSinkStore 中存储的是旧进程的采集数据，如果不清除，
    //   前端通过 /collect API 拉取时会看到旧进程的残留数据。
    //
    // 使用 shared_lock 读锁，允许并发查询但不允许并发修改。
    //
    // 参数：
    //   name:   Feature 名称
    //   params: 新的过滤参数（target_pids 和 target_comms）
    //
    // 返回：
    //   Ok:     在线重配置成功
    //   Error:  重配置失败（原因见 message）
    Status ReconfigureFilter(const std::string& name, const StartParams& params) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end() || !it->second.pipeline) {
            return Status::Error(StatusCode::kNotFound, "Feature not found or not running");
        }

        // 获取 Pipeline 的 Source 插件
        auto* source = it->second.pipeline->GetSource();
        if (!source) {
            return Status::Error(StatusCode::kInternal, "No source in pipeline");
        }
        lock.unlock();

        // 将 PID 列表转换为逗号分隔的字符串（如 "1234,5678"）
        std::string pid_str;
        for (size_t i = 0; i < params.target_pids.size(); ++i) {
            if (i > 0) pid_str += ",";
            pid_str += std::to_string(params.target_pids[i]);
        }

        // 将进程名列表转换为逗号分隔的字符串（如 "nginx,mysqld"）
        std::string comm_str;
        for (size_t i = 0; i < params.target_comms.size(); ++i) {
            if (i > 0) comm_str += ",";
            comm_str += params.target_comms[i];
        }

        // 构建新的配置并调用 Source 的 Reconfigure 方法
        ConfigValue recfg;
        recfg.Set("target_pids", pid_str);
        recfg.Set("target_comms", comm_str);
        auto st = source->Reconfigure(recfg);

        // 重配置成功后清除旧数据，避免前端看到残留的旧进程数据
        if (st.ok()) {
            StreamSinkStore::Instance().RemoveBuffer(name);
        }
        return st;
    }

    // ========================================================================
    // Stop — 停止 Feature
    // ========================================================================
    //
    // 状态转换：Active/Paused → Stopping → Inactive
    //
    // 停止流程：
    //   1. 状态校验（Feature 存在、状态合法）
    //   2. 切换状态到 Stopping（过渡状态）
    //   3. 通知外部状态变更（kActive/kPaused → kStopping）
    //   4. 停止并销毁 Pipeline：
    //      a. 注销 RecordingSink 引用（避免悬挂指针）
    //      b. 清除 StreamSinkStore 中的旧数据
    //      c. 取消所有 TimerWheel 定时器
    //      d. 调用 Pipeline::Stop()（排空 channel + 最后 flush）
    //   5. 切换状态到 Inactive
    //   6. 释放 Pipeline 实例（unique_ptr reset）
    //   7. 通知外部状态变更（kStopping → kInactive）
    //
    // 参数：
    //   name: Feature 名称
    //
    // 返回：
    //   Ok:     停止成功
    //   Error:  停止失败（原因见 message）
    Status Stop(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound,
                                 "Feature not found: " + name);
        }
        auto& entry = it->second;

        // 已 Inactive 的 Feature 直接返回成功（幂等操作）
        if (entry.state == FeatureState::kInactive) {
            return Status::Ok();
        }

        // 状态合法性检查：只能在 Active 或 Paused 状态下停止
        if (entry.state != FeatureState::kActive &&
            entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature cannot be stopped in state: " +
                                 std::string(FeatureStateToString(entry.state)));
        }

        auto prev = entry.state;
        entry.state = FeatureState::kStopping;
        lock.unlock();
        NotifyStateChange(name, prev, FeatureState::kStopping);

        // 停止并销毁 Pipeline（锁外操作）
        StopAndDestroyPipeline(name);

        lock.lock();
        entry.state = FeatureState::kInactive;
        entry.pipeline.reset();  // 释放 Pipeline 实例
        lock.unlock();
        NotifyStateChange(name, FeatureState::kStopping, FeatureState::kInactive);
        IL_INFO("Feature stopped: {}", name);
        return Status::Ok();
    }

    // ========================================================================
    // Pause — 暂停 Feature
    // ========================================================================
    //
    // 状态转换：Active → Paused
    //
    // 暂停时只取消 TimerWheel 定时器（Collect 定时器和 Flush 定时器），
    // Pipeline 线程保持运行。这允许快速恢复（Resume 只需重新注册定时器，无需重建 Pipeline）。
    //
    // 使用场景：用户从前端切换标签页时，暂停当前标签页的 Feature 以节省资源。
    //
    // 注意：暂停期间 Pipeline 的 ProcessThread 仍在运行，只是不再有新数据进入。
    // 如果 Push Source 在暂停期间推送数据，数据仍会被处理（因为 ProcessThread 未停止）。
    // 只有 Pull Source 的采集被真正暂停（定时器被取消）。
    //
    // 参数：
    //   name: Feature 名称
    //
    // 返回：
    //   Ok:     暂停成功
    //   Error:  暂停失败（Feature 不存在或不在 Active 状态）
    Status Pause(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state != FeatureState::kActive) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature must be active to pause");
        }

        // 取消所有 TimerWheel 定时器（Collect + Flush）
        PausePipeline(entry);
        entry.state = FeatureState::kPaused;
        lock.unlock();
        NotifyStateChange(name, FeatureState::kActive, FeatureState::kPaused);
        return Status::Ok();
    }

    // ========================================================================
    // Resume — 恢复 Feature
    // ========================================================================
    //
    // 状态转换：Paused → Active
    //
    // 恢复时重新注册 TimerWheel 定时器（Collect 和 Flush），不需要重建 Pipeline。
    // 这是 Pause/Resume 机制的核心优势：相比 Stop+Start 的秒级重建，恢复是毫秒级的。
    //
    // 参数：
    //   name: Feature 名称
    //
    // 返回：
    //   Ok:     恢复成功
    //   Error:  恢复失败（Feature 不存在或不在 Paused 状态）
    Status Resume(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found: " + name);
        }
        auto& entry = it->second;
        if (entry.state != FeatureState::kPaused) {
            return Status::Error(StatusCode::kInvalidArgument,
                                 "Feature must be paused to resume");
        }
        return ResumeInternal(entry);
    }

    // ========================================================================
    // ListFeatures — 列出所有 Feature 运行时快照
    // ========================================================================
    //
    // 使用 shared_lock 读锁，允许并发读取。REST API 的 GET /api/v1/features
    // 直接调用此方法获取所有 Feature 的状态信息。
    //
    // 对于每个 Feature，收集：
    //   - 静态信息：name, display_name, category, tier
    //   - 状态信息：state, is_recording
    //   - 统计信息：batches_processed, records_processed, errors
    //   - 运行时长：uptime_ms（仅在 Active 或 Paused 状态下计算）
    //
    // 返回：
    //   所有 Feature 的运行时状态快照列表
    std::vector<FeatureInfo> ListFeatures() const {
        std::shared_lock lock(mutex_);
        std::vector<FeatureInfo> result;
        result.reserve(features_.size());
        for (const auto& [name, entry] : features_) {
            FeatureInfo info;
            info.name = name;
            info.display_name = entry.config.display_name;
            info.category = entry.config.category;
            info.tier = entry.config.tier;
            info.state = entry.state;

            // 如果 Pipeline 已创建，读取运行统计
            if (entry.pipeline) {
                info.batches_processed = entry.pipeline->BatchesProcessed();
                info.records_processed = entry.pipeline->RecordsProcessed();
                info.errors = entry.pipeline->ErrorCount();

                // 通过 RecordingSinkRegistry 查询录制状态
                auto rec_sink = RecordingSinkRegistry::Instance().Get(name);
                info.is_recording = rec_sink && rec_sink->IsRecording();

                // 计算运行时长（Active 或 Paused 状态下）
                if (entry.state == FeatureState::kActive ||
                    entry.state == FeatureState::kPaused) {
                    auto elapsed = std::chrono::steady_clock::now() - entry.started_at;
                    info.uptime_ms = std::chrono::duration_cast<
                        std::chrono::milliseconds>(elapsed).count();
                }
            }
            result.push_back(std::move(info));
        }
        return result;
    }

    // ========================================================================
    // GetState — 查询指定 Feature 的当前状态
    // ========================================================================
    //
    // 使用 shared_lock 读锁，允许并发查询。
    //
    // 参数：
    //   name: Feature 名称
    //
    // 返回：
    //   Feature 的当前状态，不存在时返回 kInactive
    FeatureState GetState(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) return FeatureState::kInactive;
        return it->second.state;
    }

    // ========================================================================
    // GetPipeline — 获取指定 Feature 的 Pipeline 指针
    // ========================================================================
    //
    // 返回裸指针，不转移所有权。Pipeline 的生命周期由 FeatureEntry 管理。
    //
    // 使用 shared_lock 读锁，允许并发访问。
    //
    // 注意：调用者不应在 Stop() 期间或之后使用此指针，因为 Pipeline 可能已被销毁。
    // 仅在确认 Feature 处于 Active 或 Paused 状态时使用。
    //
    // 参数：
    //   name: Feature 名称
    //
    // 返回：
    //   Pipeline 裸指针，不存在或未启动时返回 nullptr
    Pipeline* GetPipeline(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end() || !it->second.pipeline) return nullptr;
        return it->second.pipeline.get();
    }

    // ========================================================================
    // SetStateChangeCallback — 设置状态变更回调
    // ========================================================================
    //
    // 供 WebSocketManager 使用。当 Feature 状态变化时，通过此回调推送到
    // WebSocket 客户端，前端可以实时更新 Feature 面板的状态显示。
    //
    // 例如：
    //   - 用户点击"启动"按钮 → Feature 状态变为 Starting → 前端显示"启动中..."
    //   - Pipeline 创建成功 → 状态变为 Active → 前端显示"运行中"并开始接收数据
    //   - 用户点击"停止"按钮 → 状态变为 Stopping → 前端显示"停止中..."
    //   - Pipeline 销毁完成 → 状态变为 Inactive → 前端显示"未启动"
    //
    // 参数：
    //   cb: 状态变更回调函数
    void SetStateChangeCallback(StateChangeCallback cb) {
        state_callback_ = std::move(cb);
    }

    // ========================================================================
    // StopAll — 停止所有活跃 Feature
    // ========================================================================
    //
    // 守护进程关闭时调用，先收集所有活跃 Feature 名（读锁），再逐个 Stop（写锁）。
    //
    // 为什么先收集再逐个 Stop？
    //   因为 Stop() 内部会获取 unique_lock，如果在持有读锁的情况下调用 Stop()
    //   会导致锁升级（shared_lock → unique_lock），而 std::shared_mutex 不支持锁升级。
    //   因此需要先释放读锁，再逐个获取写锁进行 Stop。
    //
    // 停止顺序：
    //   1. 收集所有 Active 或 Paused 状态的 Feature 名称
    //   2. 逐个调用 Stop()（每个 Stop 内部会获取 unique_lock）
    void StopAll() {
        std::vector<std::string> active_features;
        {
            std::shared_lock lock(mutex_);
            for (const auto& [name, entry] : features_) {
                if (entry.state == FeatureState::kActive ||
                    entry.state == FeatureState::kPaused) {
                    active_features.push_back(name);
                }
            }
        }
        for (const auto& name : active_features) {
            Stop(name);
        }
    }

private:
    // ========================================================================
    // FeatureEntry — Feature 内部存储结构
    // ========================================================================
    //
    // 每个注册的 Feature 对应一个 entry，存储在 features_ map 中。
    //
    // 字段说明：
    //   config:     Feature 的静态配置（注册时设定，不可变）
    //   state:      Feature 当前的生命周期状态
    //   pipeline:   Pipeline 实例（启动时创建，停止时销毁）
    //   started_at: 启动时间戳（用于计算 uptime_ms）
    //   timer_ids:  已注册的 TimerWheel 定时器 ID 列表
    //                - Pull Source: 1 个 Collect 定时器（按 interval_ms 周期触发）
    //                - Aggregator: 1 个 Flush 定时器（按 flush_ms 周期触发）
    //                - Push Source: 只有 Flush 定时器（无 Collect 定时器）
    //
    // 注意：timer_ids 中的定时器 ID 在 Pause 时取消、Resume 时重新注册。
    // 定时器 ID 用于 Stop 时清理所有定时器。
    struct FeatureEntry {
        FeatureConfig config;
        FeatureState state = FeatureState::kInactive;
        std::unique_ptr<Pipeline> pipeline;          // Pipeline 实例（拥有所有权）
        std::chrono::steady_clock::time_point started_at;  // 启动时间戳
        std::vector<uint32_t> timer_ids;             // 已注册的 TimerWheel 定时器 ID 列表
    };

    // ========================================================================
    // CreateAndStartPipeline — 创建并启动 Pipeline 实例
    // ========================================================================
    //
    // 这是 FeatureManager 中最核心的方法，负责从 FeatureConfig 构建完整的 Pipeline。
    //
    // 流程：
    //   1. 通过 PluginRegistry 创建 Source 插件
    //      - 根据 cfg.source.type 查找对应的工厂函数
    //      - 调用 Source::Init(source_cfg) 初始化
    //
    //   2. 合并运行时参数到 Source 配置
    //      - 将 StartParams 中的 target_pids、target_comms 合并到 source_cfg
    //      - 运行时参数优先级高于配置文件中的默认值
    //
    //   3. 通过 PluginRegistry 创建 Processor 插件
    //      - 遍历 cfg.processors 列表，逐个创建
    //      - 调用 Processor::Init(proc_cfg.config) 初始化
    //
    //   4. 通过 PluginRegistry 创建用户配置的 Sink 插件
    //      - 遍历 cfg.sinks 列表，逐个创建
    //      - 调用 Sink::Init(sink_cfg.config) 初始化
    //
    //   5. 自动注入 StreamSink（统一数据缓冲）
    //      - 所有 Feature 共享同一个 StreamSinkStore
    //      - HTTP API 和 WebSocket 都从 StreamSinkStore 读取数据
    //      - 避免了为每个 Feature 单独创建 WebSocket 连接的复杂性
    //
    //   6. 自动注入 RecordingSink 并注册到全局 Registry
    //      - RecordingSink 支持按需录制（用户通过 API 触发）
    //      - 注册到 RecordingSinkRegistry 供 API 层查询录制状态
    //      - 保存裸指针到 Registry（因为 unique_ptr 由 Pipeline 管理）
    //
    //   7. 设置 SinkPool
    //      - 从 PipelineController 获取共享的 SinkPool
    //      - SinkPool 是全局共享的，不是每个 Pipeline 独立创建
    //
    //   8. 启动 Pipeline
    //      - 按 Source → Processor → Aggregator → Sink 顺序启动所有插件
    //      - 启动 ProcessThread（事件处理线程）
    //      - 设置 Push Source 的回调（如果适用）
    //
    //   9. 注册 TimerWheel 定时器
    //      - Pull Source: 注册 Collect 定时器（按 interval_ms 周期触发采集）
    //        * 定时器回调在 TimerWheel 线程中执行
    //        * 回调提交采集任务到 CollectPool 异步执行
    //        * CollectPool 完成后调用 Pipeline::Enqueue() 推入数据
    //      - Aggregator: 注册 Flush 定时器（按 flush_ms 周期触发刷盘）
    //        * 定时器回调直接调用 Pipeline::InjectFlush() 注入 FlushSentinel
    //      - Push Source: 只注册 Flush 定时器（数据由 eBPF 回调推入，不需要 Collect 定时器）
    //
    //   10. 将 Pipeline 和 timer_ids 保存到 FeatureEntry
    //
    // 参数：
    //   name:   Feature 名称
    //   params: 启动参数（可选）
    //
    // 返回：
    //   Ok:     创建并启动成功
    //   Error:  创建或启动失败（原因见 message）
    Status CreateAndStartPipeline(const std::string& name,
                                   const StartParams& params = {}) {
        std::shared_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) {
            return Status::Error(StatusCode::kNotFound, "Feature not found");
        }
        auto& entry = it->second;
        const auto& cfg = entry.config.pipeline;
        lock.unlock();

        auto& registry = PluginRegistry::Instance();

        // 创建 Pipeline 实例
        auto pipeline = std::make_unique<Pipeline>(name);

        // ---- Step 1: 创建并初始化 Source 插件 ----
        auto source = registry.CreateSource(cfg.source.type);
        if (!source) {
            return Status::Error(StatusCode::kInternal,
                                 "Failed to create source: " + cfg.source.type);
        }

        // 合并运行时参数到 Source 配置（target_pids 覆盖配置中的默认值）
        ConfigValue source_cfg = cfg.source.config;
        if (!params.target_pids.empty()) {
            std::string pid_str;
            for (size_t i = 0; i < params.target_pids.size(); ++i) {
                if (i > 0) pid_str += ",";
                pid_str += std::to_string(params.target_pids[i]);
            }
            source_cfg.Set("target_pids", pid_str);
        }
        if (!params.target_comms.empty()) {
            std::string comm_str;
            for (size_t i = 0; i < params.target_comms.size(); ++i) {
                if (i > 0) comm_str += ",";
                comm_str += params.target_comms[i];
            }
            source_cfg.Set("target_comms", comm_str);
        }

        auto init_status = source->Init(source_cfg);
        if (!init_status.ok()) return init_status;

        pipeline->SetSource(std::move(source));

        // ---- Step 2: 创建并初始化 Processor 插件 ----
        for (const auto& proc_cfg : cfg.processors) {
            auto proc = registry.CreateProcessor(proc_cfg.type);
            if (proc) {
                proc->Init(proc_cfg.config);
                pipeline->AddProcessor(std::move(proc));
            }
        }

        // ---- Step 3: 创建并初始化用户配置的 Sink 插件 ----
        for (const auto& sink_cfg : cfg.sinks) {
            auto sink = registry.CreateSink(sink_cfg.type);
            if (sink) {
                sink->Init(sink_cfg.config);
                pipeline->AddSink(std::move(sink));
            }
        }

        // ---- Step 4: 自动注入 StreamSink ----
        // StreamSink 是 FeatureManager 自动注入的 Sink，为所有 Feature 提供统一的
        // 数据缓冲接口。WebSocketManager 直接从 StreamSinkStore 拉取数据广播，
        // 无需为每个 Feature 单独创建 WebSocket 连接。
        auto stream_sink = std::make_unique<StreamSink>();
        stream_sink->SetFeatureName(name);
        pipeline->AddSink(std::move(stream_sink));

        // ---- Step 5: 自动注入 RecordingSink 并注册到全局 Registry ----
        // RecordingSink 支持按需录制功能，用户通过 API 触发录制后，
        // 数据会写入磁盘文件（.ilr 格式），可导出和回放。
        auto rec_sink = std::make_unique<RecordingSink>();
        rec_sink->SetFeatureName(name);
        auto* rec_ptr = rec_sink.get();  // 保存裸指针，用于注册到 Registry
        pipeline->AddSink(std::move(rec_sink));
        RecordingSinkRegistry::Instance().Register(name, rec_ptr);

        // ---- Step 6: 设置共享 SinkPool ----
        // SinkPool 由 PipelineController 管理，所有 Pipeline 共享。
        // 这避免了为每个 Pipeline 单独创建线程池的资源浪费。
        pipeline->SetSinkPool(controller_.GetSinkPool());

        // ---- Step 7: 启动 Pipeline ----
        // 按 Source → Processor → Aggregator → Sink 顺序启动所有插件，
        // 然后启动 ProcessThread（事件处理线程）。
        auto start_status = pipeline->Start();
        if (!start_status.ok()) return start_status;

        // ---- Step 8: 注册 TimerWheel 定时器 ----
        auto& timer = controller_.GetTimerWheel();
        auto* src_ptr = pipeline->GetSource();
        auto* pipe_ptr = pipeline.get();

        std::vector<uint32_t> timer_ids;

        // Pull Source: 注册 Collect 定时器
        // 定时器回调在 TimerWheel 线程中执行，提交采集任务到 CollectPool 异步执行。
        // 为什么不在 TimerWheel 线程中直接调用 Collect()？
        //   - TimerWheel 线程是全局共享的调度线程，不应执行耗时操作
        //   - Collect() 可能涉及 procfs 读取、eBPF map 查询等 I/O 操作
        //   - 将 Collect 提交到 CollectPool 可以并行执行，不阻塞其他管道的调度
        if (!src_ptr->IsPushMode()) {
            uint32_t interval_ms = src_ptr->IntervalMs();
            if (interval_ms == 0) interval_ms = 1000;  // 默认 1 秒间隔
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(interval_ms),
                [this, pipe_ptr, src_ptr, name]() {
                    auto* pool = controller_.GetCollectPool();
                    if (!pool) return;
                    pool->Submit([pipe_ptr, src_ptr]() {
                        auto result = src_ptr->Collect();
                        if (result.ok() && *result && !(*result)->Empty()) {
                            pipe_ptr->Enqueue(std::move(*result));
                        }
                    });
                }
            );
            timer_ids.push_back(tid);
        }

        // Aggregator: 注册 Flush 定时器
        // 定时器回调直接调用 Pipeline::InjectFlush()，向 AsyncChannel 注入
        // FlushSentinel。ProcessThread 收到 FlushSentinel 后调用 Aggregator::Flush()
        // 将累积的数据通过 swap 操作快速取出，提交到 SinkPool 写入。
        if (pipe_ptr->HasAggregator()) {
            uint32_t flush_ms = pipe_ptr->FlushIntervalMs();
            if (flush_ms == 0) flush_ms = 3000;  // 默认 3 秒间隔
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(flush_ms),
                [pipe_ptr]() { pipe_ptr->InjectFlush(); }
            );
            timer_ids.push_back(tid);
        }

        // ---- Step 9: 保存 Pipeline 和定时器 ID 到 FeatureEntry ----
        std::unique_lock wlock(mutex_);
        auto wit = features_.find(name);
        if (wit != features_.end()) {
            wit->second.pipeline = std::move(pipeline);
            wit->second.timer_ids = std::move(timer_ids);
        }
        return Status::Ok();
    }

    // ========================================================================
    // StopAndDestroyPipeline — 停止并销毁 Pipeline
    // ========================================================================
    //
    // 停止流程：
    //   1. 注销 RecordingSink 引用（避免悬挂指针）
    //      - RecordingSinkRegistry 中存储的是裸指针
    //      - Pipeline 销毁后裸指针失效，必须先注销
    //   2. 清除 StreamSinkStore 中的旧数据
    //      - 避免重启后返回过期采样
    //   3. 取消所有 TimerWheel 定时器
    //      - Collect 定时器（Pull Source）
    //      - Flush 定时器（Aggregator）
    //   4. 调用 Pipeline::Stop()
    //      - Source::Stop() → 排空 channel → 最后 flush → Processor/Sink Stop
    //      - 等待 ProcessThread 退出
    //
    // 参数：
    //   name: Feature 名称
    void StopAndDestroyPipeline(const std::string& name) {
        // 先注销 RecordingSink 引用（避免悬挂指针）
        RecordingSinkRegistry::Instance().Unregister(name);

        // 清除 StreamSinkStore 中的旧数据，避免重启后返回过期采样
        StreamSinkStore::Instance().RemoveBuffer(name);

        std::unique_lock lock(mutex_);
        auto it = features_.find(name);
        if (it == features_.end()) return;
        auto& entry = it->second;

        // 取消所有 TimerWheel 定时器（Collect + Flush）
        auto& timer = controller_.GetTimerWheel();
        for (auto tid : entry.timer_ids) {
            timer.Cancel(tid);
        }
        entry.timer_ids.clear();

        // 停止 Pipeline（锁外调用 Pipeline::Stop()）
        if (entry.pipeline) {
            lock.unlock();
            entry.pipeline->Stop();  // 排空 channel + 最后 flush + 等待 ProcessThread 退出
            lock.lock();
        }
    }

    // ========================================================================
    // PausePipeline — 暂停 Pipeline（取消定时器）
    // ========================================================================
    //
    // 取消所有 TimerWheel 定时器，但保持 Pipeline 线程运行。
    // 定时器被取消后，Pull Source 不再有新的 Collect 触发，Aggregator 不再有 Flush 触发。
    // Push Source 不受影响（数据由 eBPF 回调推入，不依赖定时器）。
    //
    // 注意：此方法不改变 Feature 状态，状态变更由调用者（Pause()）负责。
    //
    // 参数：
    //   entry: FeatureEntry 引用（调用者必须持有锁）
    void PausePipeline(FeatureEntry& entry) {
        auto& timer = controller_.GetTimerWheel();
        for (auto tid : entry.timer_ids) {
            timer.Cancel(tid);
        }
        entry.timer_ids.clear();
    }

    // ========================================================================
    // ResumeInternal — 恢复 Pipeline（重新注册定时器）
    // ========================================================================
    //
    // 重新注册 TimerWheel 定时器（Collect + Flush），恢复数据采集。
    //
    // 恢复流程：
    //   1. 检查 Pipeline 是否仍在运行
    //   2. Pull Source：重新注册 Collect 定时器
    //   3. 切换状态到 Active
    //   4. 通知外部状态变更
    //
    // 注意：此方法在锁内调用，因为 entry 引用来自调用者持有的锁。
    //
    // 参数：
    //   entry: FeatureEntry 引用（调用者必须持有锁）
    //
    // 返回：
    //   Ok:     恢复成功
    //   Error:  Pipeline 未运行，无法恢复
    Status ResumeInternal(FeatureEntry& entry) {
        if (!entry.pipeline || !entry.pipeline->IsRunning()) {
            return Status::Error(StatusCode::kInternal,
                                 "Pipeline not running for resume");
        }

        auto& timer = controller_.GetTimerWheel();
        auto* src_ptr = entry.pipeline->GetSource();
        auto* pipe_ptr = entry.pipeline.get();
        const auto& name = entry.config.name;

        // Pull Source: 重新注册 Collect 定时器
        if (!src_ptr->IsPushMode()) {
            uint32_t interval_ms = src_ptr->IntervalMs();
            if (interval_ms == 0) interval_ms = 1000;  // 默认 1 秒间隔
            auto tid = timer.AddRepeating(
                std::chrono::milliseconds(interval_ms),
                [this, pipe_ptr, src_ptr, name]() {
                    auto* pool = controller_.GetCollectPool();
                    if (!pool) return;
                    pool->Submit([pipe_ptr, src_ptr]() {
                        auto result = src_ptr->Collect();
                        if (result.ok() && *result && !(*result)->Empty()) {
                            pipe_ptr->Enqueue(std::move(*result));
                        }
                    });
                }
            );
            entry.timer_ids.push_back(tid);
        }

        // 切换状态到 Active 并通知外部
        entry.state = FeatureState::kActive;
        NotifyStateChange(name, FeatureState::kPaused, FeatureState::kActive);
        return Status::Ok();
    }

    // ========================================================================
    // NotifyStateChange — 通知外部状态变更
    // ========================================================================
    //
    // 如果设置了 state_callback_，则调用回调通知外部状态变更。
    // 回调通常在锁外调用，避免回调中再次获取锁导致的死锁。
    //
    // 参数：
    //   name: Feature 名称
    //   from: 变更前的状态
    //   to:   变更后的状态
    void NotifyStateChange(const std::string& name, FeatureState from, FeatureState to) {
        if (state_callback_) {
            state_callback_(name, from, to);
        }
    }

    // ========================================================================
    // 成员变量
    // ========================================================================

    // PipelineController 引用（不拥有所有权）
    // FeatureManager 通过它访问 TimerWheel、CollectPool、SinkPool 等共享基础设施。
    // controller_ 的生命周期必须长于 FeatureManager。
    PipelineController& controller_;

    // 读写锁（mutable 允许在 const 方法中获取写锁）
    // 使用 shared_mutex 实现读多写少优化：
    //   - ListFeatures、GetState、GetPipeline 使用 shared_lock（允许多并发读）
    //   - Start、Stop、Pause、Resume、RegisterFeature 使用 unique_lock（互斥写）
    mutable std::shared_mutex mutex_;

    // Feature 存储映射
    // key: Feature 名称（如 "cpu_utilization"）
    // value: FeatureEntry（包含配置、状态、Pipeline 实例）
    std::unordered_map<std::string, FeatureEntry> features_;

    // 状态变更回调
    // 由 WebSocketManager 设置，用于推送状态更新到前端。
    StateChangeCallback state_callback_;
};

}  // namespace illuminator