# eBPF 插件基类重设计方案

> **状态**: 已实现 (Implemented)
> **日期**: 2026-07-25
> **作者**: Architecture Review
> **范围**: `src/plugin/features/ebpf_source_base.h`, `src/ebpf_common/loader/bpf_util.h`, 及所有 eBPF Source 插件
>
> **实现记录**:
> - 所有 6 个 eBPF 插件已迁移至 `EbpfSourceBase`
> - 旧模板 `EbpfSkeletonSource` 和 `EbpfSkeletonPullSource` 已删除
> - 114 个构建目标编译通过，34 个单元测试全部通过
> - 总 eBPF 代码量从 3033 行减少至 2048 行 (-32%)

---

## 1. 现状分析

### 1.1 当前架构

现有 eBPF Source 插件基于两个独立的模板类：

```
SourcePlugin (plugin_api.h)
├── EbpfSkeletonSource<SkelOps>       ← Push 模式模板 (321 行)
├── EbpfSkeletonPullSource<SkelOps>   ← Pull 模式模板 (481 行)
├── CpuProfilerSource                 ← 直接继承 SourcePlugin (924 行)
└── SchedAnalyzerSource               ← 直接继承 SourcePlugin (582 行)
```

### 1.2 存在的问题

#### 问题 1：三种实现风格，缺乏统一性

| 插件 | 基类 | 行数 | 模式 |
|------|------|------|------|
| EbpfIoMonitor | `EbpfSkeletonSource<>` | 67 | Push |
| EbpfNetTracer | `EbpfSkeletonSource<>` | 73 | Push |
| EbpfSchedTracer | `EbpfSkeletonSource<>` | 73 | Push |
| OffcpuProfiler | `EbpfSkeletonPullSource<>` | 519 | Pull |
| **CpuProfilerSource** | **SourcePlugin** | **924** | Dual (手写) |
| **SchedAnalyzerSource** | **SourcePlugin** | **582** | Dual (手写) |

模板只覆盖了简单场景。复杂插件绕过了整个模板体系，导致：
- 同样的 skeleton 生命周期管理代码出现三遍
- 同样的 PID 过滤逻辑出现三遍
- Bug 修复和能力增强需要同步到多处

#### 问题 2：两个模板之间大量重复

`EbpfSkeletonSource` 和 `EbpfSkeletonPullSource` 共享以下逻辑，但完全独立实现：
- skeleton open → configure → load → attach → destroy 生命周期
- stub 模式降级
- `MetaStats` / `GetBpfStats()` 读取
- ring buffer 创建和 poll 线程管理
- `running_` / `paused_` / `stub_mode_` 状态管理

#### 问题 3：每个 eBPF 插件创建独立的 poll 线程

当前设计为每个 Push 模式 eBPF 插件创建一个独立的 `std::thread`，在其中调用
`ring_buffer__poll(rb, 100)`。如果有 N 个 Push 插件同时运行，就有 N 个 poll 线程。

**为什么当前需要 poll 线程？**

`ring_buffer__poll()` 是 libbpf 提供的阻塞调用，内部使用 `epoll_wait` 等待 BPF ring buffer
中有新数据可读。它的工作方式是：

1. BPF 程序通过 `bpf_ringbuf_submit()` 向 ring buffer 写入事件
2. 写入会唤醒 epoll，`ring_buffer__poll()` 返回
3. libbpf 调用注册的回调函数处理事件
4. 回调完成后继续等待

因为 `ring_buffer__poll()` 是阻塞的，所以需要一个线程来驻留等待。

**问题在于**：这不是唯一的设计选择，而且当前的每插件独立线程模式存在明显的资源浪费。

#### 问题 4：PID/Comm 过滤、PID Namespace 硬编码在基类中

当前 `EbpfSkeletonPullSource` 将以下逻辑直接写在模板基类里：
- PID 过滤 map 管理（`WritePidFilter`, `ClearAndRewritePidMap`）
- 进程名过滤 map 管理（`WriteCommFilter`, `ClearAndRewriteCommMap`）
- PID Namespace 配置（`ConfigurePidNamespace`）
- cfg bit-flag 构建（`BuildCfgFlags`）

但这些不是所有 eBPF 插件的通用需求。例如：
- 系统级指标插件（如 memory pressure）不需要 PID 过滤
- 网络插件可能需要 IP/端口过滤而不是 PID 过滤
- 磁盘 I/O 插件可能需要设备过滤

**设计原则**：基类应该只包含所有 eBPF 插件共有的能力，过滤是一种可组合的策略。

---

## 2. 设计目标

1. **一个公共基类**：提取所有 eBPF Source 的不变逻辑，消除重复
2. **可组合的能力**：过滤策略、配置参数等作为独立 mixin/trait，按需组合
3. **统一的 attach 抽象**：覆盖 tracepoint、kprobe、perf_event 等不同挂载方式
4. **共享 poll 机制**：消除 per-plugin poll 线程，使用共享的事件驱动
5. **声明式配置**：参数定义、校验、Schema 生成自动化
6. **复杂插件也能用框架**：CpuProfiler 和 SchedAnalyzer 应该能用基类 + hooks 实现

---

## 3. 架构设计

### 3.1 总体层次结构

```
SourcePlugin (plugin_api.h)
│
└── EbpfSourceBase                    ← 新增：eBPF 不变逻辑基类
     │  • skeleton 生命周期 (open/load/attach/destroy)
     │  • stub 模式 & 内核特性探测
     │  • BPF stats (meta_stats)
     │  • 公共状态管理 (running/paused/stub)
     │  • 统一的 Start/Stop/Pause/Resume 框架
     │
     ├── [+PidFilter]                ← 可选 mixin：PID/Comm 过滤能力
     ├── [+PerfEventAttach]          ← 可选 mixin：perf_event 挂载能力
     └── [+DualMode]                 ← 可选 mixin：Push/Pull 双模式切换
```

注意：不使用传统的多层模板继承（如 `EbpfPushSource`, `EbpfPullSource`），
而是使用 **hooks + mixins** 模式，让基类通过虚函数回调来适应不同场景。

### 3.2 核心基类：`EbpfSourceBase`

#### 3.2.1 设计哲学

借鉴 Linux 内核驱动框架的核心模式：

```
驱动框架 = 不变逻辑（生命周期管理） + ops 回调表（变化点由驱动填充）
```

例如 Linux 的 `struct net_device_ops`：框架负责注册、初始化、sysfs 暴露；
驱动只需填写 `ndo_open`, `ndo_stop`, `ndo_start_xmit` 等回调。

`EbpfSourceBase` 的角色就是这个**框架层**——管理所有 eBPF 插件共有的生命周期，
将差异点暴露为受控的 hook 点。

#### 3.2.2 基类提供的能力（不变逻辑）

这些是所有 eBPF Source 插件都需要的，由基类统一管理：

| 能力 | 说明 | 当前问题 |
|------|------|----------|
| Skeleton 生命周期 | open → configure_rodata → load → configure_maps → attach → destroy | 三处重复 |
| Stub 模式 | 内核不支持 BPF 时优雅降级 | 三处重复 |
| MetaStats | PERCPU_ARRAY 自观测统计读取 | 三处重复 |
| 状态管理 | `running_`, `paused_`, `stub_mode_` 原子状态 | 三处重复 |
| Start/Stop 框架 | 标准化的启动/关闭序列 | 三处不同实现 |
| Pause/Resume 框架 | 暂停/恢复采集 | 每个插件自己实现 |

#### 3.2.3 Hook 点（变化逻辑，子类填充）

这些是各 eBPF 插件不同的部分，通过虚函数暴露：

```cpp
class EbpfSourceBase : public SourcePlugin {
protected:
    // ================================================================
    // Skeleton Hooks — 子类必须实现
    // ================================================================

    // 返回 skeleton 操作的函数指针集合
    // 类似 Linux 的 struct xxx_ops
    struct SkelCallbacks {
        void* (*open)();
        int   (*load)(void* skel);
        int   (*attach)(void* skel);
        void  (*destroy)(void* skel);
        int   (*ringbuf_map_fd)(void* skel);        // -1 if none
        int   (*gate_map_fd)(void* skel);            // -1 if none
        int   (*meta_stats_map_fd)(void* skel);      // -1 if none
    };

    virtual SkelCallbacks GetSkelCallbacks() const = 0;

    // ================================================================
    // Configuration Hooks — 子类可选覆写
    // ================================================================

    // Phase 1: open() 之后、load() 之前 — 写入 rodata
    virtual void OnConfigureRodata(void* skel) {}

    // Phase 2: load() 之后、attach() 之前 — 写入 BPF maps
    virtual void OnConfigureMaps(void* skel) {}

    // Phase 3: attach() 之后 — 创建额外的挂载点（如 perf_event）
    virtual Status OnPostAttach(void* skel) { return Status::Ok(); }

    // Phase 4: 停止时 — 清理子类创建的额外资源
    virtual void OnPreDestroy() {}

    // ================================================================
    // Data Hooks — 子类必须实现（至少一个）
    // ================================================================

    // Push 模式：返回 ring buffer 事件回调
    // 返回 nullptr 表示不使用 Push 模式
    virtual ring_buffer_sample_fn GetEventCallback() const { return nullptr; }

    // Pull 模式：从 BPF maps 读取聚合数据
    // 返回空指针表示不使用 Pull 模式
    virtual StatusOr<DataBatchPtr> CollectFromMaps() {
        return Status::Error(StatusCode::kUnimplemented, "Pull mode not supported");
    }

    // ================================================================
    // Runtime Hooks — 子类可选覆写
    // ================================================================

    // 运行时重配置（子类专有参数）
    virtual Status OnReconfigure(const ConfigValue& params) {
        return Status::Ok();
    }

    // Pause 时的子类特殊处理
    virtual void OnPause() {}

    // Resume 时的子类特殊处理
    virtual void OnResume() {}
};
```

#### 3.2.4 Start/Stop 标准流程

基类的 `Start()` 实现一个标准化的启动序列，子类通过 hooks 注入差异：

```
Start():
  1. 内核特性探测 → stub mode if unsupported
  2. skel = callbacks.open()
  3. OnConfigureRodata(skel)       ← hook: rodata 配置
  4. callbacks.load(skel)
  5. OnConfigureMaps(skel)         ← hook: BPF map 初始化
  6. callbacks.attach(skel)
  7. OnPostAttach(skel)            ← hook: perf_event 等额外挂载
  8. 初始化 ring buffer (if ringbuf_map_fd >= 0)
  9. 初始化 meta_stats fd
  10. 注册到 BpfPollService (if push mode)  ← 共享 poll
  11. running_ = true
```

```
Stop():
  1. running_ = false
  2. 从 BpfPollService 注销 (if push mode)
  3. OnPreDestroy()                ← hook: 清理额外资源
  4. 释放 ring buffer
  5. callbacks.destroy(skel)
```

### 3.3 消除独立 Poll 线程：复用 TimerWheel + CollectPool

#### 3.3.1 当前问题

每个 Push 模式 eBPF 插件创建独立的 poll 线程：

```
[io-poll]  ──── ring_buffer__poll(rb_io, 100)     // 阻塞等待
[net-poll] ──── ring_buffer__poll(rb_net, 100)     // 阻塞等待
[sched-poll] ── ring_buffer__poll(rb_sched, 100)   // 阻塞等待
```

问题：
- **线程开销**：7 个 eBPF 插件 = 7 个线程，每个线程 8KB+ 栈空间 + 调度开销
- **与 Linux 理念冲突**：Linux 用 NAPI、softirq、workqueue 共享处理，
  不为每个设备驱动独立创建中断线程

#### 3.3.2 为什么当前设计使用 poll 线程？

`ring_buffer__poll(rb, timeout_ms)` 底层调用 `epoll_wait`，这是一个**阻塞调用**：

```c
int ring_buffer__poll(struct ring_buffer *rb, int timeout_ms) {
    // 阻塞在 epoll_wait，等待 BPF 侧写入 ring buffer 触发唤醒
    int cnt = epoll_wait(rb->epoll_fd, events, rb->ring_cnt, timeout_ms);
    // 处理事件，调用回调
    for (int i = 0; i < cnt; i++)
        ringbuf_process_ring(ring);
    return total;
}
```

因为是阻塞的，不能直接提交到 CollectPool——否则一个 poll 任务会独占一个线程。

#### 3.3.3 关键洞察：libbpf 提供了非阻塞的 `ring_buffer__consume()`

```c
// ring_buffer__poll()    — 阻塞：调用 epoll_wait 等待事件
// ring_buffer__consume() — 非阻塞：处理已有的事件，立即返回
int ring_buffer__consume(struct ring_buffer *rb);
```

`consume()` 不调用 `epoll_wait`。它直接扫描 ring buffer 中已到达的数据，
处理完后立即返回。**零阻塞，可安全在 CollectPool 中执行**。

#### 3.3.4 新方案：TimerWheel 定时驱动 + CollectPool 执行 consume + 批量打包

将 Push 模式和 Pull 模式统一到同一条路径：

```
Pull 模式（不变）：
  TimerWheel 每 1s → CollectPool → source.Collect() → 读 BPF map → Pipeline

Push 模式（改为定时排空 ring buffer + 批量打包）：
  BPF 事件 → 在 ring buffer 中排队（无需立即处理）
  TimerWheel 每 20-50ms → CollectPool → ring_buffer__consume(rb)
    → 一次性排空所有排队事件 → 打包成一个 DataBatch → Pipeline
```

核心设计：**ring buffer 充当事件队列，定时批量排空**。
事件在两次 consume 之间自然排队，overflow 通过 `meta_stats` 计数跟踪。
对于性能采集这类非延迟敏感系统，这是最合理的选择。

实现：

```cpp
// EbpfSourceBase 中
void RegisterPushTimer(InfrastructureManager& infra) {
    push_timer_id_ = infra.GetTimerWheel().AddRepeating(
        std::chrono::milliseconds(consume_interval_ms_),  // 20-50ms
        [this, &infra] {
            if (!running_.load() || paused_.load()) return;
            infra.GetCollectPool()->Submit([this] {
                ConsumeAndBatch();
                return 0;
            });
        }
    );
}

// 批量消费：一次 consume 期间所有事件打包到一个 batch
void ConsumeAndBatch() {
    if (!ring_buf_) return;
    pending_batch_ = MakeNewBatch();       // 子类决定 batch 类型
    ring_buffer__consume(ring_buf_);       // 非阻塞！调用 N 次 HandleEvent
    if (pending_batch_ && !pending_batch_->Empty())
        pipeline_->Enqueue(std::move(pending_batch_));  // 一次 enqueue
}
```

子类的 HandleEvent 不再单独推送，只添加到当前 batch：

```cpp
static int HandleEvent(void* ctx, void* data, size_t size) {
    auto* self = static_cast<EbpfIoMonitor*>(ctx);
    auto* event = static_cast<il_bio_event*>(data);
    // 添加到当前 pending batch，不单独推送
    auto& rec = self->pending_batch_->AddRecord();
    // ... 填充 record ...
    return 0;
}
```

**批量打包的性能优势**（假设 50ms 间隔内积攒 500 个事件）：

| 指标 | 当前（per-event push） | 改进后（batch drain） |
|------|----------------------|---------------------|
| DataBatch 分配 | 500 次 | **1 次** |
| Pipeline enqueue | 500 次 | **1 次** |
| AsyncChannel 竞争 | 500 次 atomic op | **1 次** |
| shared_ptr 引用计数 | 500 次 atomic inc/dec | **1 次** |

**Ring buffer 溢出分析**（以 I/O 监控为例）：

```
高负载场景：~10K 事件/sec × 64 bytes/event = 640KB/sec
Ring buffer 4MB ÷ 640KB/sec = 可缓冲约 6 秒
Drain 间隔 50ms → 实际缓冲 32KB → 远低于 ring buffer 容量

即使溢出：
  BPF 侧 bpf_ringbuf_reserve() 返回 NULL → 事件丢弃
  meta_stats.buffer_full++ → 前端 FeatureHealthBadge 可观测
  对性能采集场景完全可接受（损失精度但保持可用性）
```
```

**效果**：
- **零额外线程**：完全复用 TimerWheel + CollectPool
- **Push 和 Pull 走同一条路**：从框架层面看，两者只是定时间隔和数据读取方式不同
- **延迟可控**：consume 间隔 10-50ms（可配置，默认 20ms）
- **无阻塞**：`consume()` 非阻塞，不会占用 CollectPool 线程

#### 3.3.5 延迟分析

| 环节 | 延迟 |
|------|------|
| consume 间隔 | 10-50ms（可配置） |
| AsyncChannel 入队 | <1μs |
| ProcessThread 出队 | <100μs |
| SinkPool → SseSink | <1ms |
| SSE 网络传输 | <10ms |
| 前端 rAF 批处理 | ~16ms |
| **端到端** | **~40-80ms** |

前端 DataBus 本身就有 100ms 的 batch flush 间隔。10-50ms 的 consume 延迟
对用户完全不可感知。

#### 3.3.6 何时才需要真正的 epoll 即时唤醒？

只有极低延迟场景才需要独立 poll 线程：
- 实时安全告警（<1ms 响应要求）
- 低延迟交易系统的 eBPF 监控

对于 Illuminator 这样的可观测性仪表盘平台，20ms 的 consume 间隔完全足够。
如果未来需要极低延迟模式，可以作为可选能力提供（在基类中
`virtual bool NeedsImmediatePoll() const { return false; }`），
但默认使用 TimerWheel 驱动。

#### 3.3.7 Pull 模式中 Ring Buffer 信号的处理

当前 `EbpfSkeletonPullSource` 也创建了一个 poll 线程，用于接收 ring buffer
中的"信号"事件。去掉 poll 线程后，信号 ring buffer 用 Collect() 中的
非阻塞 consume 处理即可：

```cpp
StatusOr<DataBatchPtr> Collect() override {
    // 非阻塞消费一下信号（清空 ring buffer 中的通知）
    if (ring_buf_) ring_buffer__consume(ring_buf_);
    // 直接从 BPF maps 读取聚合数据
    return ReadAndClearStats();
}
```

### 3.4 配置接口设计：统一 hook + 工具函数

#### 3.4.1 设计原则

借鉴 Linux 驱动框架的真实做法：
- `ndo_set_features()` 是一个统一回调，驱动根据 `changed` 标志位分发不同配置逻辑
- `devm_request_irq()`, `dma_alloc_coherent()` 等是**独立的工具函数**，
  驱动按需调用，不强制使用
- 驱动不是通过继承获得 DMA 能力，而是主动调用 DMA 工具函数

**核心思想**：
- 基类只提供统一的配置 hook 接口（`OnConfigure` / `OnReconfigure`）
- 常用的 BPF map 操作封装为独立的**工具函数**（不是类），放在 `bpf_util` 命名空间
- 子类在自己的 hook 实现中，根据配置字段分发，按需调用工具函数

**不使用独立辅助类**（如 `BpfPidFilter`），原因：
1. 多一层抽象增加理解成本
2. 辅助类需要维护自己的状态，但 BPF map fd 等状态生命周期由 skeleton 管理
3. 工具函数更灵活——插件可以选择调用部分函数，不需要整套

#### 3.4.2 工具函数设计

```cpp
// bpf_util.h — 独立工具函数，不参与任何继承链
namespace illuminator::bpf_util {

// ---- PID/Comm 过滤相关 ----

// 将 PID 列表写入 BPF hash map (key=pid, val=1)
void WritePidFilter(int map_fd, const std::vector<uint32_t>& pids);

// 将进程名列表写入 BPF hash map (key=comm[16], val=1)
void WriteCommFilter(int map_fd, const std::vector<std::string>& comms);

// 清空 BPF hash map 中所有条目
void ClearBpfHashMap(int map_fd);

// 清空后重写（用于 Reconfigure）
void RewritePidFilter(int map_fd, const std::vector<uint32_t>& pids);
void RewriteCommFilter(int map_fd, const std::vector<std::string>& comms);

// ---- PID Namespace 配置 ----

// 将当前进程的 PID namespace dev/ino 写入 BPF map
// 供 BPF 侧 bpf_get_ns_current_pid_tgid() 使用
void ConfigurePidNamespace(int pidns_map_fd);

// ---- perf_event 管理 ----

struct PerfConfig {
    uint32_t type = PERF_TYPE_SOFTWARE;
    uint64_t config = PERF_COUNT_SW_CPU_CLOCK;
    uint64_t sample_freq = 49;
    bool freq_mode = true;
    bool exclude_user = false;
    bool exclude_kernel = false;
};

// 为所有在线 CPU 创建 perf_event 并绑定 BPF 程序
// 返回创建成功的 perf_event fd 列表
std::vector<int> AttachPerfEvents(int prog_fd, const PerfConfig& cfg);

// 关闭所有 perf_event fd
void DetachPerfEvents(std::vector<int>& fds);

// 暂停/恢复所有 perf_event
void DisablePerfEvents(const std::vector<int>& fds);
void EnablePerfEvents(const std::vector<int>& fds);

// 调整 perf_event 采样频率（反压响应）
void AdjustPerfFrequency(const std::vector<int>& fds, uint64_t new_freq);

// ---- 在线 CPU ----
std::vector<int> ParseOnlineCpuIds();

}  // namespace bpf_util
```

#### 3.4.3 子类的配置实现示例

**CpuProfilerSource**（需要 PID 过滤 + perf_event）：

```cpp
class CpuProfilerSource : public EbpfSourceBase {
    Status OnConfigure(const ConfigValue& config, void* s) override {
        auto* sk = static_cast<skel_t*>(s);

        // 根据配置字段分发不同功能
        auto pids = ParseCommaSeparated<uint32_t>(config["target_pids"].AsString(""));
        if (!pids.empty())
            bpf_util::WritePidFilter(bpf_map__fd(sk->maps.target_pids), pids);

        auto comms = ParseCommaSeparated<std::string>(config["target_comms"].AsString(""));
        if (!comms.empty())
            bpf_util::WriteCommFilter(bpf_map__fd(sk->maps.target_comms), comms);

        bpf_util::ConfigurePidNamespace(bpf_map__fd(sk->maps.cpu_pidns_cfg));

        // 配置标志位
        uint32_t flags = (stream_mode_ ? 1u : 0u)
                       | (!pids.empty() ? 2u : 0u)
                       | (!comms.empty() ? 4u : 0u);
        WriteCfgMap(bpf_map__fd(sk->maps.cpu_profiler_cfg), flags);

        return Status::Ok();
    }

    Status OnPostAttach(void* s) override {
        auto* sk = static_cast<skel_t*>(s);
        bpf_util::PerfConfig cfg{
            .sample_freq = static_cast<uint64_t>(frequency_hz_),
            .exclude_user = !user_stacks_,
            .exclude_kernel = !kernel_stacks_,
        };
        perf_fds_ = bpf_util::AttachPerfEvents(
            bpf_program__fd(sk->progs.on_cpu_sample), cfg);
        return perf_fds_.empty()
            ? Status::Error(StatusCode::kInternal, "no perf events")
            : Status::Ok();
    }

    Status OnReconfigure(const ConfigValue& params) override {
        // 根据字段分发：PID 变了就重写 PID map，频率变了就需要 restart
        auto pid_str = params["target_pids"].AsString("");
        if (!pid_str.empty()) {
            auto pids = ParseCommaSeparated<uint32_t>(pid_str);
            bpf_util::RewritePidFilter(pid_map_fd_, pids);
        }
        return Status::Ok();
    }
};
```

**EbpfIoMonitor**（不需要 PID 过滤）：

```cpp
class EbpfIoMonitor : public EbpfSourceBase {
    Status OnConfigure(const ConfigValue& config, void* s) override {
        // I/O 监控不需要任何额外配置
        // 未来可添加设备过滤：
        // auto devices = config["target_devices"].AsString("");
        // if (!devices.empty()) ConfigureDeviceFilter(sk, devices);
        return Status::Ok();
    }
};
```

#### 3.4.4 双模式支持

双模式（Push+Pull）通过基类的 hook 机制自然支持：

```cpp
class SchedAnalyzerSource : public EbpfSourceBase {
    bool detailed_mode_ = false;

    bool IsPushMode() const override { return detailed_mode_; }

    ring_buffer_sample_fn GetEventCallback() const override {
        return detailed_mode_ ? HandleDetailedEvent : nullptr;
    }

    StatusOr<DataBatchPtr> CollectFromMaps() override {
        return ReadSchedAggMap();
    }
};
```

基类根据 `IsPushMode()` 和 `GetEventCallback()` 的返回值自动决定：
- 是否注册定时 consume 任务（Push 模式）或定时 Collect 任务（Pull 模式）
- Pause/Resume 时是否操作 BPF gate

### 3.5 `SkelOps` 宏的改进

#### 3.5.1 当前问题

`IL_DEFINE_SKEL_OPS` 宏生成一个静态 traits 结构。它可以工作，但有几个不足：
- 宏参数固定（ringbuf map 名、gate map 名），不够灵活
- 无法表达"没有 ring buffer"或"没有 gate map"的情况（只能传 dummy）
- 类型安全通过宏拼接实现，编译错误信息不友好

#### 3.5.2 改进方案

改用 `SkelCallbacks` 结构体 + 生成宏：

```cpp
// 新的宏：生成 SkelCallbacks 构造函数
#define IL_SKEL_CALLBACKS(skel_prefix)                                    \
    EbpfSourceBase::SkelCallbacks MakeSkelCallbacks() const override {    \
        return {                                                          \
            .open    = [](){ return (void*)skel_prefix##_bpf__open(); },  \
            .load    = [](void* s){ return skel_prefix##_bpf__load(      \
                            (skel_prefix##_bpf*)s); },                    \
            .attach  = [](void* s){ return skel_prefix##_bpf__attach(    \
                            (skel_prefix##_bpf*)s); },                    \
            .destroy = [](void* s){ skel_prefix##_bpf__destroy(          \
                            (skel_prefix##_bpf*)s); },                    \
        };                                                                \
    }                                                                     \
    using skel_t = struct skel_prefix##_bpf

// 子类使用：
class EbpfIoMonitor : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(bio_latency_sk);

    // 单独覆写需要的 map fd 访问
    int GetRingBufMapFd() const override {
        return bpf_map__fd(skel()->maps.bio_events);
    }
    int GetGateMapFd() const override {
        return bpf_map__fd(skel()->maps.collection_gate);
    }
    int GetMetaStatsMapFd() const override {
        return bpf_map__fd(skel()->maps.meta_stats);
    }
};
```

**或者更简洁**，使用 CRTP 让子类直接声明 map 名：

```cpp
class EbpfIoMonitor : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(bio_latency_sk);

protected:
    // 通过 skel() 获取类型安全的 skeleton 指针
    skel_t* skel() { return static_cast<skel_t*>(raw_skel_); }

    void OnConfigureMaps(void* s) override {
        auto* sk = static_cast<skel_t*>(s);
        SetRingBufFd(bpf_map__fd(sk->maps.bio_events));
        SetGateFd(bpf_map__fd(sk->maps.collection_gate));
        SetMetaStatsFd(bpf_map__fd(sk->maps.meta_stats));
    }
};
```

### 3.6 声明式配置参数

#### 3.6.1 当前问题

各插件的配置参数散落在 `Init()` 方法中手动解析，`ConfigSchema()` 返回手写的
JSON 字符串。新增参数需要同时修改 Init、Reconfigure、ConfigSchema 三处。

#### 3.6.2 改进方案

引入声明式参数定义，一处定义、多处自动使用：

```cpp
// 声明式参数定义（编译期常量）
struct EbpfParamDef {
    const char* key;
    ParamType type;
    const char* display_name;
    const char* description;
    const char* default_value;
    int min_val, max_val;        // for integer/range
    const char* choices;         // for enum, comma-separated
    bool requires_restart;       // rodata 参数需要 restart
};

// 示例：CpuProfiler 的参数声明
static constexpr EbpfParamDef kCpuProfilerParams[] = {
    {"sample_freq",     ParamType::kInteger,  "采样频率",
     "Sampling frequency in Hz", "49", 1, 4999, nullptr, true},

    {"user_stacks",     ParamType::kBoolean,  "用户态堆栈",
     "Collect user-space stacks", "true", 0, 0, nullptr, true},

    {"kernel_stacks",   ParamType::kBoolean,  "内核态堆栈",
     "Collect kernel stacks", "true", 0, 0, nullptr, true},

    {"mode",            ParamType::kEnum,     "工作模式",
     "Operating mode", "aggregated", 0, 0, "aggregated,stream", true},

    {"target_pids",     ParamType::kPidList,  "目标进程",
     "Filter by PID list", "", 0, 0, nullptr, false},

    {"target_process_names", ParamType::kString, "目标进程名",
     "Filter by process name", "", 0, 0, nullptr, false},
};
```

基类自动提供：

```cpp
class EbpfSourceBase {
    // 子类覆写，返回自己的参数声明表
    virtual std::span<const EbpfParamDef> ParamDefs() const { return {}; }

    // 自动生成 JSON Schema（给前端用）
    std::string ConfigSchema() const override {
        return GenerateJsonSchema(ParamDefs());
    }

    // 自动校验并解析配置
    Status Init(const ConfigValue& config) override {
        auto status = ValidateAndParseParams(config, ParamDefs());
        if (!status.ok()) return status;
        return OnInit(config);  // 子类 hook
    }

    // Reconfigure 时自动识别需要 restart 的参数
    Status Reconfigure(const ConfigValue& params) override {
        auto [runtime_params, restart_params] = ClassifyParams(params, ParamDefs());
        if (!restart_params.empty()) {
            return Status::Error(StatusCode::kRequiresRestart,
                "Parameters require restart: " + Join(restart_params));
        }
        return OnReconfigure(runtime_params);
    }
};
```

---

## 4. 改进后效果对比

### 4.1 CpuProfilerSource 重构前后

**重构前：924 行，直接继承 SourcePlugin**

```
手动管理：
  - skeleton open/load/attach/destroy     (~50 行)
  - perf_event_open + ioctl               (~80 行)
  - PID/Comm 过滤 map 写入                 (~60 行)
  - PID Namespace 配置                     (~25 行)
  - Reconfigure（清空+重写 maps）           (~80 行)
  - RebuildPerfEvents                     (~45 行)
  - Start 流程编排                         (~120 行)
  - Stop 清理                             (~30 行)
  - Pause/Resume (stream+aggregated)      (~60 行)
  - StreamPollLoop / AggregatedPullLoop   (~35 行)
  - poll 线程管理                          (~15 行)
  - SnapshotAggregatedCounts              (~40 行)
  - FlushAggregatedCounts                 (~40 行)
  - HandleStreamEvent                     (~35 行)
  - BuildJsonSnapshot                     (~35 行)
  - OnBackpressure                        (~15 行)
  - Init 参数解析                          (~30 行)
  - 成员变量声明                           (~25 行)
  - 工具函数                              (~50 行)
```

**重构后：~180 行，继承 EbpfSourceBase + 调用 bpf_util 工具函数**

```cpp
class CpuProfilerSource : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(cpu_profiler_sk);

    const char* Name() const override { return "cpu_profiler"; }
    const char* Version() const override { return "0.2.0"; }
    bool IsPushMode() const override { return stream_mode_; }

    // ---- Skeleton hooks ----
    void OnConfigureRodata(void* s) override {
        auto* sk = static_cast<skel_t*>(s);
        if (sk->rodata)
            sk->rodata->sample_freq = static_cast<uint64_t>(frequency_hz_);
    }

    // 统一配置接口：根据字段分发不同功能
    Status OnConfigure(const ConfigValue& config, void* s) override {
        auto* sk = static_cast<skel_t*>(s);

        // PID 过滤（按需）
        auto pids = ParseCommaSeparated<uint32_t>(config["target_pids"].AsString(""));
        if (!pids.empty())
            bpf_util::WritePidFilter(bpf_map__fd(sk->maps.target_pids), pids);

        // 进程名过滤（按需）
        auto comms = ParseCommaSeparated<std::string>(config["target_comms"].AsString(""));
        if (!comms.empty())
            bpf_util::WriteCommFilter(bpf_map__fd(sk->maps.target_comms), comms);

        // PID Namespace
        bpf_util::ConfigurePidNamespace(bpf_map__fd(sk->maps.cpu_pidns_cfg));

        // 配置标志位
        uint32_t flags = (stream_mode_ ? 1u : 0u)
                       | (!pids.empty() ? 2u : 0u)
                       | (!comms.empty() ? 4u : 0u);
        WriteCfgMap(bpf_map__fd(sk->maps.cpu_profiler_cfg), flags);

        // perf_event 挂载
        bpf_util::PerfConfig perf_cfg{
            .sample_freq = static_cast<uint64_t>(frequency_hz_),
            .exclude_user = !user_stacks_,
            .exclude_kernel = !kernel_stacks_,
        };
        perf_fds_ = bpf_util::AttachPerfEvents(
            bpf_program__fd(sk->progs.on_cpu_sample), perf_cfg);
        return perf_fds_.empty()
            ? Status::Error(StatusCode::kInternal, "no perf events")
            : Status::Ok();
    }

    void OnPreDestroy() override { bpf_util::DetachPerfEvents(perf_fds_); }
    void OnPause() override { bpf_util::DisablePerfEvents(perf_fds_); }
    void OnResume() override { bpf_util::EnablePerfEvents(perf_fds_); }

    // ---- 数据处理 ----
    ring_buffer_sample_fn GetEventCallback() const override {
        return stream_mode_ ? HandleStreamEvent : nullptr;
    }

    StatusOr<DataBatchPtr> CollectFromMaps() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        SnapshotAggregatedCounts(batch.get());
        return batch;
    }

    // ---- 反压 ----
    void OnBackpressure(bool active) override {
        bpf_util::AdjustPerfFrequency(perf_fds_,
            active ? frequency_hz_ / 4 : frequency_hz_);
    }

    // ---- 重配置：根据字段分发 ----
    Status OnReconfigure(const ConfigValue& params) override {
        auto pid_str = params["target_pids"].AsString("");
        if (!pid_str.empty()) {
            auto pids = ParseCommaSeparated<uint32_t>(pid_str);
            bpf_util::RewritePidFilter(pid_map_fd_, pids);
        }
        // ... 其他字段 ...
        return Status::Ok();
    }

private:
    static int HandleStreamEvent(void* ctx, void* data, size_t size) { ... }
    void SnapshotAggregatedCounts(DataBatch* batch) { ... }

    int frequency_hz_ = 49;
    bool stream_mode_ = false;
    bool user_stacks_ = true;
    bool kernel_stacks_ = true;
    int pid_map_fd_ = -1;
    std::vector<int> perf_fds_;
};
```

### 4.2 EbpfIoMonitor 重构前后

**重构前（67 行）：**
需要 `IL_DEFINE_SKEL_OPS_WITH_META` 宏 + 继承 `EbpfSkeletonSource<>`

**重构后（~45 行）：**

```cpp
class EbpfIoMonitor : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(bio_latency_sk);

    const char* Name() const override { return "ebpf_io_monitor"; }
    const char* Version() const override { return "0.2.0"; }

    Status OnConfigure(const ConfigValue& config, void* s) override {
        auto* sk = static_cast<skel_t*>(s);
        SetRingBufFd(bpf_map__fd(sk->maps.bio_events));
        SetGateFd(bpf_map__fd(sk->maps.collection_gate));
        SetMetaStatsFd(bpf_map__fd(sk->maps.meta_stats));
        return Status::Ok();
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return HandleEvent;
    }

    static int HandleEvent(void* ctx, void* data, size_t size) {
        // ... 事件转 DataBatch 的业务逻辑（不变）...
    }
};
```

---

## 5. 迁移计划（已完成）

### Phase 1：基础设施 ✅

1. ✅ 实现 `EbpfSourceBase` 基类 → `src/plugin/features/ebpf_source_base.h` (354 行)
2. ✅ 实现 `bpf_util` 工具函数 → `src/ebpf_common/loader/bpf_util.h` (225 行)
3. 最终设计：不使用 `BpfPidFilter`/`BpfPerfAttach` 独立类，改为 `bpf_util` 工具函数

### Phase 2：迁移简单插件 ✅

1. ✅ `EbpfIoMonitor` → `EbpfSourceBase` (70 行)
2. ✅ `EbpfNetTracer` → `EbpfSourceBase` (81 行)
3. ✅ `EbpfSchedTracer` → `EbpfSourceBase` (82 行)

### Phase 3：迁移复杂插件 ✅

1. ✅ `CpuProfilerSource` → `EbpfSourceBase` + `bpf_util` (337 行, 原 923 行 → -63%)
2. ✅ `OffcpuProfilerSource` → `EbpfSourceBase` + `bpf_util` (496 行, 原 518 行)
3. ✅ `SchedAnalyzerSource` → `EbpfSourceBase` (403 行, 原 581 行 → -31%)

### Phase 4：清理 ✅

1. ✅ 删除 `EbpfSkeletonSource` 和 `EbpfSkeletonPullSource` 旧模板
2. ✅ 删除所有 `.legacy.h` 备份文件
3. ✅ 更新 BUILD 文件
4. ✅ 更新 `architecture_overview.md`
5. ✅ 114 targets 编译通过，34 tests 全部通过

---

## 6. 设计决策总结

| 决策 | 选择 | 理由 |
|------|------|------|
| 基类 vs 模板 | 非模板基类 + hooks | 模板导致两份基类代码；hooks 更灵活 |
| 过滤机制 | **统一配置 hook + 工具函数** | 基类只提供 `OnConfigure` hook，PID/Comm/设备等过滤作为工具函数按需调用，不强制绑定 |
| Poll 线程 | **消除**，复用 TimerWheel + CollectPool | `ring_buffer__consume()` 非阻塞，Push/Pull 统一走 TimerWheel 定时驱动 |
| Pull 模式信号 RB | Collect() 中非阻塞 consume | 不值得为此创建线程 |
| PerfEvent 管理 | **工具函数** (`bpf_util::AttachPerfEvents`) | 不需要独立类；插件在 `OnConfigure` 中按需调用 |
| 配置参数定义 | **声明式表**（可选） | 一处定义，Schema/Init/Reconfigure 自动 |
| Attach 抽象 | Hook 点 (`OnPostAttach` 或 `OnConfigure`) | 不同插件的 attach 方式差异太大，不适合统一接口 |

---

## 7. 与 Linux 驱动框架的对应关系

| Linux 驱动框架概念 | Illuminator 对应 | 说明 |
|---|---|---|
| `struct bus_type` | `FeatureBus` | 设备总线，管理所有驱动的注册/probe |
| `struct device_driver` | `FeatureDriver` | 驱动抽象，拥有生命周期 |
| `struct device` | `SourcePlugin` / 具体 eBPF 插件 | 具体的设备实例 |
| `struct xxx_ops` | `SkelCallbacks` + hooks | 驱动需要填充的回调表 |
| `driver_register()` | `REGISTER_FEATURE()` | 驱动注册到总线 |
| `probe()` | `FeatureDriver::Probe()` | 探测并初始化设备 |
| `remove()` | `FeatureDriver::Remove()` | 移除设备 |
| `suspend/resume` | `Pause()` / `Resume()` | 电源管理 |
| sysfs attributes | REST API + `ConfigSchema()` | 用户态配置接口 |
| devicetree / ACPI | 声明式 `EbpfParamDef[]` | 硬件描述 / 配置描述 |
| `devm_` 资源管理 | 基类 Start/Stop 自动管理 | 设备管理的资源自动释放 |
| 中断处理 (NAPI/softirq) | TimerWheel + CollectPool + consume() | 共享的事件处理，不为每个设备创建线程 |
| 工具宏 (`module_platform_driver`) | `IL_SKEL_CALLBACKS` | 减少样板代码 |
| 能力标志 (`NETIF_F_*`) | `IsPushMode()` / `HasBpfProbe()` | 设备能力声明 |
| 工具函数 (`devm_request_irq`, `dma_alloc_coherent`) | `bpf_util::WritePidFilter` 等 | 驱动按需调用的独立工具函数 |
| `ndo_set_features` 统一回调 | `OnConfigure(config, skel)` | 一个回调根据字段分发不同配置 |

---

## 附录 A：文件变更清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `src/plugin/features/ebpf_source_base.h` | eBPF Source 公共基类 |
| `src/plugin/features/bpf_util.h` | BPF 操作工具函数（PID 过滤、perf_event、PID namespace 等） |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/core/engine/infrastructure_manager.h` | 新增 `BpfPollService` 成员 |
| `src/plugin/features/io/ebpf_io_monitor.h` | 迁移到 `EbpfSourceBase` |
| `src/plugin/features/net/ebpf_net_tracer.h` | 迁移到 `EbpfSourceBase` |
| `src/plugin/features/sched/ebpf_sched_tracer.h` | 迁移到 `EbpfSourceBase` |
| `src/plugin/features/sched/offcpu_profiler.h` | 迁移到 `EbpfSourceBase` + `BpfPidFilter` |
| `src/plugin/features/cpu/cpu_profiler.h` | 迁移到 `EbpfSourceBase` + `BpfPerfAttach` + `BpfPidFilter` |
| `src/plugin/features/sched/sched_analyzer.h` | 迁移到 `EbpfSourceBase` |

### 删除文件（Phase 4）

| 文件 | 说明 |
|------|------|
| `src/plugin/features/ebpf_skeleton_source.h` | 被 `EbpfSourceBase` 取代 |
| `src/plugin/features/ebpf_skeleton_pull_source.h` | 被 `EbpfSourceBase` 取代 |
