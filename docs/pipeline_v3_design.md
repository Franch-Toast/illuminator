# Illuminator Pipeline v3 — 事件驱动异步管道架构设计

> **版本**: 3.0  
> **状态**: ✅ 已实现（2026-05 完成）  
> **设计理念**: 调度与执行分离 · 单一职责线程 · Actor 式状态隔离  
> **实现文件**: `src/core/engine/pipeline.h` + `timer_wheel.h` + `feature_driver.h` (Pipeline 生命周期由 FeatureDriver::Probe/Remove 管理)

> **架构位置说明** (2026-07-02 更新): Pipeline v3 机制仍是 Illuminator 数据处理的核心引擎。  
> 在 RFC v3 三层架构中，每个 `FeatureDriver` 通过 `BuildPipeline()` 方法创建自己的 Pipeline 实例。  
> `InfrastructureManager` 提供 TimerWheel、CollectPool、SinkPool 等共享基础设施。  
> `PipelineController` 已删除。Daemon 和 CLI collect 均通过 FeatureBus → FeatureDriver → Pipeline 管理生命周期。

---

## 一、设计目标

1. **每个线程只做一件事** — 消除 ProcessThread 兼任 flush 轮询的职责混淆
2. **采集并行安全** — 一个慢 `Collect()` 不阻塞其他 Pull Source
3. **事件驱动** — 所有定时行为由事件触发，非轮询检查
4. **零锁处理路径** — Processor 和 Aggregator 由独占线程串行访问，无需加锁
5. **I/O 隔离** — 所有 I/O 操作（Collect, Write）在池化线程中执行，与数据处理解耦

## 二、开源项目对标

| 项目 | 语言 | 调度 | 组件隔离 | Processor 执行 | flush 触发 |
|------|------|------|---------|--------------|-----------|
| **Vector** | Rust | Tokio work-stealing | async task per component | 同步调用链 | tokio::time::interval |
| **OTel Collector** | Go | goroutine per stage | Go channel | 同步调用链 | time.Ticker |
| **Telegraf** | Go | goroutine per plugin | Go channel | 同步 Accumulator | Output 独立 Ticker |
| **Fluent Bit** | C | mk_event_loop | ring buffer | 同步 | event loop timer |
| **Prometheus** | Go | goroutine per target | 无(直接 append) | — | Ticker |
| **Alloy** | Go | goroutine per component | Go channel | 同步 | 组件内 Ticker |

**六大项目共性提炼**：

1. 采集永远并行（每个 Source 独立执行，互不阻塞）
2. Processor 链同步串行（有状态组件不跨线程）
3. flush 由独立定时器事件触发（不是处理循环中的 if-check）
4. 组件间通过有界通道(channel)连接
5. I/O 操作与数据处理分离

## 三、架构总览

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          Illuminator Pipeline v3                         │
│                                                                          │
│  ┌────────────────────────────────────┐                                  │
│  │     TimerWheel  (1 thread)         │  ← 全局统一调度器                 │
│  │                                    │     只决定"何时触发什么"           │
│  │     内部: timerfd + epoll + eventfd│     不执行任何实际工作             │
│  │     存储: priority_queue<TimerEntry>│                                 │
│  │                                    │                                  │
│  │     注册的事件:                     │                                  │
│  │     ├─ CollectEvent(pipe_A, @1s)   │─── 到时 → 提交 Collect 到 Pool   │
│  │     ├─ CollectEvent(pipe_B, @2s)   │─── 到时 → 提交 Collect 到 Pool   │
│  │     ├─ FlushEvent(pipe_A, @3s)     │─── 到时 → 注入 Sentinel 到 Ch    │
│  │     ├─ FlushEvent(pipe_C, @5s)     │─── 到时 → 注入 Sentinel 到 Ch    │
│  │     ├─ MetricsSync(@10s)           │─── 到时 → 提交 Sync 到 Pool      │
│  │     └─ PruneStorage(@60s)          │─── 到时 → 提交 Prune 到 SinkPool │
│  └────────────────────────────────────┘                                  │
│                     │                                                    │
│            ┌────────┴─────────┐                                          │
│            │                  │                                          │
│            ▼                  ▼                                          │
│  ┌──────────────────┐  ┌──────────────────────────────────────────────┐  │
│  │ CollectPool       │  │ Per Pipeline (× N):                         │  │
│  │ (M threads, 共享)  │  │                                            │  │
│  │                   │  │  ┌────────────────────────────────────────┐ │  │
│  │ 执行:              │  │  │ AsyncChannel(ChannelItem, capacity)   │ │  │
│  │ src.Collect()     │──┼─→│ item = variant<DataBatchPtr, Sentinel> │ │  │
│  │                   │  │  │ 三级自适应退避出队: spin→yield→sleep   │ │  │
│  │ 结果入队到各管道   │  │  └──────────────────┬─────────────────────┘ │  │
│  │ 的 AsyncChannel   │  │                     │                       │  │
│  │                   │  │                     ▼                       │  │
│  └──────────────────┘  │  ┌────────────────────────────────────────┐ │  │
│                         │  │ ProcessThread (1 per pipeline)         │ │  │
│  Push Sources:          │  │                                        │ │  │
│  eBPF callback ─────────┼─→│ 纯事件处理器 (Event Handler):           │ │  │
│  → channel.Enqueue()    │  │  match item:                          │ │  │
│                         │  │    DataBatch  → RunProcessors()       │ │  │
│                         │  │               → Aggregator.Add()      │ │  │
│                         │  │               → SubmitToSinks()       │ │  │
│                         │  │    Sentinel   → Aggregator.Flush()    │ │  │
│                         │  │               → SubmitToSinks()       │ │  │
│                         │  │                                        │ │  │
│                         │  │ 每100次循环: 同步指标 + 检查资源限制     │ │  │
│                         │  │ 保证: 纯 CPU-bound, 不做任何 I/O       │ │  │
│                         │  └────────────────────────┬───────────────┘ │  │
│                         │                           │                  │  │
│                         └───────────────────────────┼──────────────────┘  │
│                                                     │                     │
│  ┌──────────────────────────────────────────────────▼──────────────────┐  │
│  │ SinkPool (K threads, 共享)                                          │  │
│  │ 执行: sink.Write()                                                   │  │
│  │ 职责: 所有 I/O-bound 写入操作                                         │  │
│  │ 过载保护: 待处理任务 > 256 → 丢弃数据                                 │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐ │
│  │ FeatureBus + FeatureDriver — 功能级生命周期管理（1 Feature = 1 Pipeline）│ │
│  │                                                                      │ │
│  │ 状态机 (FeatureDriver): Inactive → Probe() → Active ⇄ Paused → Remove()  │
│  │ 分级:   Tier 1 (Monitoring) 自动 Probe | Tier 2 (Tracing) 自动 Probe  │ │
│  │        Tier 3 (Profiling) 手动触发                                    │ │
│  │ 自动注入: SseSink (SSE 实时推送) + RecordingSink (录制回放)           │ │
│  │ Supported: 按需启停 / 暂停恢复 / 录制 / 重配过滤 / 状态回调 / 安全校验   │ │
│  │ 注: PipelineController 已删除，CLI collect 同样使用 FeatureBus         │ │
│  └──────────────────────────────────────────────────────────────────────┘ │
│                                                                          │
│  线程总计: 1 (TimerWheel) + M (CollectPool) + N (ProcessThread) + K (Sink)│
│  典型 10 管道: 1 + 2 + 10 + 4 = 17 线程                                  │
└──────────────────────────────────────────────────────────────────────────┘
```

## 四、组件详细设计

### 4.1 TimerWheel — 全局定时事件调度器

**职责**: 管理所有周期性事件的触发时机。不执行任何实际工作。

**设计参考**: Fluent Bit 的 `mk_event_loop` + Kafka 的 `TimingWheel`

**实际实现**: 使用 `timerfd` + `epoll` + `eventfd`（Linux 原生机制），非 `condition_variable` 方案。

```
TimerWheel 内部三大组件：

┌──────────────────────────────────────────────────┐
│              TimerWheel 线程 (timer-wheel)         │
│                                                  │
│  ┌──────────┐   ┌──────────┐   ┌──────────────┐ │
│  │ timerfd  │   │ eventfd  │   │ priority_queue│ │
│  │(内核定时器)│   │(唤醒信号) │   │   (最小堆)    │ │
│  └────┬─────┘   └────┬─────┘   └──────┬───────┘ │
│       │              │                │          │
│       └──────┬───────┘                │          │
│              │                        │          │
│        ┌─────▼─────┐                  │          │
│        │   epoll   │◄─────────────────┘          │
│        │ (多路复用) │  堆顶的到期时间 arm timerfd   │
│        └─────┬─────┘                             │
│              │                                   │
│        epoll_wait 阻塞等待 + 1s 超时              │
│              │                                   │
│   timerfd到期 / eventfd被写 / 超时                │
│              │                                   │
│        ProcessFired() → 弹出到期条目 → 执行回调    │
│              │                                   │
│        RearmTimerfdLocked() → 重新 arm timerfd    │
└──────────────────────────────────────────────────┘
```

**运行流程**:

1. **注册定时器**（`AddRepeating`/`AddOnce`）：创建 `TimerEntry` 推入最小堆 → 重新 arm timerfd 到堆顶时间 → 写 eventfd 唤醒 epoll 循环
2. **主循环**（`Run`）：`epoll_wait` 阻塞等待 timerfd 到期或 eventfd 唤醒 → 收到事件后调用 `ProcessFired()`
3. **触发回调**（`ProcessFired`）：加锁弹出所有到期条目 → **释放锁** → 逐个执行回调（锁外执行，防止死锁）→ 重复定时器 `next_fire += interval` 后重新入堆 → 重新 arm timerfd
4. **取消定时器**（`Cancel`）：只记录 ID 到 `cancelled_` 列表（O(1)），下次 `ProcessFired` 时通过 `PurgeCancelledLocked` 重建堆统一清理（懒惰删除）

**关键设计决策**:

| 决策 | 选择 | 理由 |
|------|------|------|
| 实现方式 | `timerfd` + `epoll` + `eventfd` | Linux 内核精度高、无忙等待、支持外部唤醒 |
| 回调执行 | 释放锁后执行回调 | 防止回调中注册新定时器导致死锁 |
| 线程数 | 固定 1 线程 | 回调只做 `Submit/Enqueue`，纳秒级完成 |
| 取消策略 | 懒惰删除（Lazy Deletion） | O(1) 取消，下次触发时统一清理 |
| 重复定时器 drift | `next_fire += interval` | 避免回调执行时间累积导致的漂移 |
| epoll 超时 | 1 秒超时 | 兜底检查，防止边界情况下的死等 |

> **注**: 原设计评审考虑了 `priority_queue + cv::wait_until` 方案（跨平台），
> 最终实现选择了 `timerfd + epoll`（更高精度、支持 `eventfd` 唤醒注册新定时器）。
> 详见 `src/core/engine/timer_wheel.h`。

### 4.2 AsyncChannel — 支持 variant 的管道通道

**改造点**: 将 `LockFreeQueue<DataBatchPtr>` 扩展为 `LockFreeQueue<ChannelItem>`，支持数据和 sentinel 两种消息。

**核心设计**: 事件多态 + 三级自适应退避 + 滞后反压 + 丢包策略。

```
AsyncChannel 内部结构：

┌──────────────────────────────────────────────────────────────────┐
│                        AsyncChannel                              │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  LockFreeQueue<ChannelItem> (无锁环形缓冲区)                │  │
│  │                                                            │  │
│  │  variant<DataBatchPtr, FlushSentinel>                     │  │
│  │  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐      │  │
│  │  │ D1 │ D2 │  F │ D3 │ D4 │  F │ D5 │ D6 │ D7 │  F │ ...  │  │
│  │  └────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘      │  │
│  │   D=DataBatch(指标/堆栈)  F=FlushSentinel(刷盘信号)         │  │
│  │                                                            │  │
│  │  容量: 默认 4096 (对齐到 2 的幂)                            │  │
│  │  内存: 4096 × 32B ≈ 128KB                                 │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  生产端:                         消费端:                          │
│  ┌──────────────┐               ┌──────────────────────────┐    │
│  │ TryEnqueue() │               │ Dequeue() 三级退避        │    │
│  │              │               │                          │    │
│  │ 1. CAS 推入  │               │ Phase 1: Spin 16 次      │    │
│  │ 2. 满→丢包   │               │ Phase 2: Yield 8 次      │    │
│  │ 3. 更新反压  │               │ Phase 3: Sleep 1ms×N     │    │
│  └──────────────┘               └──────────────────────────┘    │
│                                                                  │
│  ┌──────────────┐               ┌──────────────────────────┐    │
│  │ InjectFlush()│               │ TryDequeue() 非阻塞       │    │
│  │              │               │                          │    │
│  │ 优先级高于   │               │ 用于 Drain 优雅停机      │    │
│  │ 普通数据     │               │ 立即返回，不等待         │    │
│  │ 队列满时驱逐 │               └──────────────────────────┘    │
│  │ 旧数据腾空间 │                                               │
│  └──────────────┘                                               │
│                                                                  │
│  反压机制 (滞后设计):                                             │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  [空] ──────→ 20% (low) ──────→ 80% (high) ──────→ [满]  │  │
│  │   ↑              ↑                  ↑               ↑      │  │
│  │   正常         解除反压          触发反压         全部丢弃   │  │
│  │                                                            │  │
│  │  滞后 (hysteresis) 避免在阈值附近反复震荡：                  │  │
│  │  - 进入反压: 超过 80% (high watermark)                     │  │
│  │  - 退出反压: 低于 20% (low watermark)                      │  │
│  │  - 在 20%-80% 之间: 保持当前状态不变                        │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

**三级自适应退避详解**:

```
数据到达频率高           数据到达频率中           数据到达频率低/空队列
      │                        │                        │
Phase 1: Spin 16次     Phase 2: Yield 8次      Phase 3: Sleep 1ms×N
      │                        │                        │
  ┌───▼────┐            ┌─────▼─────┐           ┌──────▼──────┐
  │ 忙等    │            │ 让出 CPU  │           │ 睡眠 1ms    │
  │ 不间断  │            │ 给其他线程 │           │ 期间检查    │
  │ TryPop  │            │ TryPop    │           │ TryPop      │
  └───┬────┘            └─────┬─────┘           └──────┬──────┘
      │                       │                        │
   延迟: ~几十ns            延迟: ~几μs              延迟: ~几ms
   开销: 100% CPU           开销: 让出 CPU           开销: 接近 0% CPU
```

| 退避阶段 | 策略 | 尝试次数 | 延迟 | CPU 开销 | 适用场景 |
|---------|------|---------|------|---------|---------|
| Phase 1 | Spin（忙等） | 16 次 | ~几十纳秒 | 100% 单核 | 高频数据（eBPF 事件流） |
| Phase 2 | Yield（让出 CPU） | 8 次 | ~几微秒 | 低 | 中频数据（procfs 采集） |
| Phase 3 | Sleep（睡眠） | 直到超时 | ~几毫秒 | ~0% | 低频/空队列（空闲管道） |

**丢包策略对比**:

| 策略 | 行为 | 适用场景 |
|------|------|---------|
| `kDropNewest` | 拒绝新数据，保留旧数据 | 数据新鲜度优先，历史数据更有价值 |
| `kDropOldest` | 弹出旧数据，为新数据腾空间 | 最新数据优先，旧数据可以被丢弃 |

**InjectFlush 优先级设计**:

```
普通数据入队 (TryEnqueue):
  队列满 → 根据丢包策略决定（丢弃或驱逐）

FlushSentinel 入队 (InjectFlush):
  队列满 → 驱逐一个旧数据（无论策略） → 重新尝试入队
  Sentinel 是控制信号，必须送达，否则 Aggregator 中的数据永远无法刷出
```

**反压信号传递链**:

```
AsyncChannel::UpdateBackpressure()
  → backpressured_ = true
      ↓
Pipeline::Enqueue() 检测到 backpressured_ 状态变化
  → source_->OnBackpressure(true)
      ↓
SourcePlugin::OnBackpressure()
  → 降低采集频率 / 丢弃低优先级数据
      ↓
队列水位下降 → UpdateBackpressure() → backpressured_ = false
  → source_->OnBackpressure(false) → 恢复正常采集
```

**variant 对 LockFreeQueue 的影响分析**:

```
sizeof(DataBatchPtr) = sizeof(shared_ptr<DataBatch>) = 16 bytes
sizeof(FlushSentinel) = 1 byte
sizeof(ChannelItem) = sizeof(variant<16B, 1B>) = 24 bytes (含 discriminant + padding)

每个 Cell = atomic<size_t>(8B) + ChannelItem(24B) = 32 bytes
总内存 = 4096 × 32 = 128 KB (与之前相同量级，可接受)
```

### 4.3 ProcessThread — 纯事件处理器

**职责**: 从 channel 消费事件，串行执行有状态逻辑（Processor 链 + Aggregator），提交结果到 SinkPool。

**核心约束**: **不做任何 I/O，不持有任何定时器，不做任何调度决策。**

**实际实现**（`pipeline.h` 中的 `Pipeline::ProcessLoop`）:

```cpp
void ProcessLoop() {
    uint32_t loop_count = 0;

    while (running_.load(std::memory_order_acquire)) {
        // 三级自适应退避出队：spin → yield → sleep(1ms)
        auto item = ingest_channel_.Dequeue(std::chrono::milliseconds(100));
        if (!item) {
            if (++loop_count % 100 == 0) SyncChannelMetrics();
            continue;
        }

        // variant 分发：DataBatch 或 FlushSentinel
        std::visit(Overloaded{
            [this](DataBatchPtr& batch) {
                HandleData(std::move(batch));
            },
            [this](FlushSentinel&) {
                HandleFlush();
            },
        }, *item);

        // 每 100 次循环同步一次指标 + 检查资源限制
        if (++loop_count % 100 == 0) {
            SyncChannelMetrics();
            auto usage = ResourceLimiter::Instance().Check();
            if (usage.memory_exceeded) {
                IL_WARN("Pipeline '{}': memory limit exceeded (RSS={} bytes)",
                        name_, usage.rss_bytes);
            }
        }
    }

    Drain();  // 退出前排空 channel 中剩余数据
}

void HandleData(DataBatchPtr batch) {
    records_processed_.fetch_add(batch->Size(), std::memory_order_relaxed);

    // Processor 链：同步串行，微秒级
    for (auto& proc : processors_) {
        auto result = proc->Process(std::move(batch));
        if (!result.ok()) {
            error_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        batch = std::move(result.value());
        if (!batch || batch->Empty()) return;
    }

    // 分发：有 Aggregator 则累积，否则直接到 Sink
    if (aggregator_) {
        aggregator_->Add(std::move(batch));
    } else {
        SubmitToSinks(std::move(batch));
    }

    batches_processed_.fetch_add(1, std::memory_order_relaxed);
}

void HandleFlush() {
    if (!aggregator_) return;

    auto result = aggregator_->Flush();
    if (result.ok()) {
        for (auto& batch : result.value()) {
            SubmitToSinks(std::move(batch));
        }
    }
}

// 优雅停机：排空 channel 中所有剩余数据 + 最后一次 flush
void Drain() {
    while (auto item = ingest_channel_.TryDequeue()) {
        std::visit(Overloaded{
            [this](DataBatchPtr& batch) { HandleData(std::move(batch)); },
            [this](FlushSentinel&) { HandleFlush(); },
        }, *item);
    }
    if (aggregator_) HandleFlush();  // 最后 flush 一次聚合器中的残留数据
}
```

**Sink 提交（带背压保护）**:

```cpp
void SubmitToSinks(DataBatchPtr batch) {
    if (!sink_pool_) {
        // 无池回退：直接写（仅单 Sink 场景可接受）
        for (auto& sink : sinks_) sink->Write(batch);
        return;
    }

    // SinkPool 过载保护：待处理任务超过 256 时丢弃数据
    static constexpr size_t kMaxPendingTasks = 256;
    if (sink_pool_->PendingTasks() > kMaxPendingTasks) {
        IL_WARN("Pipeline '{}': SinkPool overloaded ({} pending), dropping batch",
                name_, sink_pool_->PendingTasks());
        error_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    for (auto& sink : sinks_) {
        sink_pool_->Submit([sink_ptr = sink.get(), batch, this]() -> void {
            auto status = sink_ptr->Write(batch);
            if (!status.ok()) {
                IL_WARN("Sink write error in pipeline '{}': {}",
                        name_, status.message());
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
}
```

### 4.4 CollectPool — 共享 I/O 工作池

**职责**: 并行执行所有 `Source::Collect()` 调用。

**为什么不复用 SinkPool?** 
- 隔离性：一个 Sink 写入超时不应影响 Source 采集
- 可观测性：分开统计 Collect 和 Write 的排队/执行时间
- 配置独立：Collect 通常需要 2-3 线程，Sink 可能需要 4-8 线程

**实现**: 直接复用现有 `ThreadPool`，与 SinkPool 是两个独立实例。

```cpp
// InfrastructureManager 中:
std::unique_ptr<ThreadPool> collect_pool_;  // M 线程，用于 Collect
std::unique_ptr<ThreadPool> sink_pool_;     // K 线程，用于 Write

// TimerWheel 注册 Pull Source 采集事件:
for (auto& [pipeline, source] : pull_sources) {
    timer_.AddRepeating(
        std::chrono::milliseconds(source->IntervalMs()),
        [this, pipeline, source] {
            collect_pool_->Submit([pipeline, source] {
                auto result = source->Collect();
                if (result.ok() && *result && !(*result)->Empty()) {
                    pipeline->Enqueue(std::move(*result));
                }
            });
        }
    );
}
```

### 4.5 SinkPool — 共享写入池

**职责**: 并行执行 `Sink::Write()`。

**与当前实现的区别**: 所有 Sink 写入都通过 SinkPool，包括单 Sink 场景。当前的"单 Sink 快路径"（直接在 ProcessThread 中写）会导致 ProcessThread 被 I/O 阻塞。

```cpp
// 配置
struct EngineConfig {
    uint32_t collect_pool_threads = 0;  // 0 = auto (2 threads)
    uint32_t sink_pool_threads = 0;     // 0 = auto (CPU/2 threads)
    // ...
};
```

## 五、数据流时序图

### 5.1 Pull Source 正常数据流

```
TimerWheel        CollectPool       AsyncChannel      ProcessThread       SinkPool
    │                  │                  │                  │                │
    │── timer fires ──┐                  │                  │                │
    │                  │                  │                  │                │
    │  回调: Submit(Collect)              │                  │                │
    │─────────────────→│                  │                  │                │
    │                  │                  │                  │                │
    │                  │ src.Collect()    │                  │                │
    │                  │───────┐          │                  │                │
    │                  │       │ 读 /proc │                  │                │
    │                  │←──────┘          │                  │                │
    │                  │                  │                  │                │
    │                  │ TryEnqueue(Data) │                  │                │
    │                  │────────────────→│                  │                │
    │                  │                  │                  │                │
    │                  │                  │ Dequeue(Data)    │                │
    │                  │                  │ (spin→yield→sleep)│               │
    │                  │                  │─────────────────→│                │
    │                  │                  │                  │                │
    │                  │                  │                  │ variant match  │
    │                  │                  │                  │ → HandleData() │
    │                  │                  │                  │───────┐        │
    │                  │                  │                  │ RunProcessors()│
    │                  │                  │                  │ agg.Add()      │
    │                  │                  │                  │←──────┘        │
    │                  │                  │                  │                │
    │                  │                  │                  │ SubmitToSinks()│
    │                  │                  │                  │───────────────→│
    │                  │                  │                  │                │
    │                  │                  │                  │                │ sink.Write()
    │                  │                  │                  │                │───────┐
    │                  │                  │                  │                │       │ I/O
    │                  │                  │                  │                │←──────┘
```

### 5.2 FlushSentinel 触发 Aggregator 输出

```
TimerWheel        AsyncChannel      ProcessThread       SinkPool
    │                  │                  │                │
    │── flush timer ──→│                  │                │
    │  InjectFlush()   │                  │                │
    │                  │                  │                │
    │                  │ Dequeue(Sentinel)│                │
    │                  │─────────────────→│                │
    │                  │                  │                │
    │                  │                  │ agg.Flush()    │
    │                  │                  │───────┐        │
    │                  │                  │←──────┘        │
    │                  │                  │ (swap, μs级)   │
    │                  │                  │                │
    │                  │                  │ Submit(Write)  │
    │                  │                  │───────────────→│
    │                  │                  │                │
    │                  │                  │                │ sink.Write()
    │                  │                  │                │───────┐
    │                  │                  │                │       │ I/O
    │                  │                  │                │←──────┘
```

### 5.3 Push Source (eBPF) 数据流

```
eBPF callback       AsyncChannel      ProcessThread       SinkPool
    │                    │                  │                │
    │ TryEnqueue(Data)   │                  │                │
    │───────────────────→│                  │                │
    │ (无锁 CAS, ns级)   │                  │                │
    │                    │  Dequeue(Data)   │                │
    │                    │─────────────────→│                │
    │                    │                  │                │
    │                    │                  │ ProcessChain() │
    │                    │                  │ SubmitToSinks()│
    │                    │                  │───────────────→│
```

## 六、线程阻塞安全分析

| 组件 | 可能阻塞的操作 | 最大阻塞时间 | 是否影响其他管道 | 缓解措施 |
|------|--------------|-------------|----------------|---------|
| **TimerWheel** | `wait_until()` | 直到下一个 timer | 否（不执行实际工作） | — |
| **CollectPool Worker** | `Source::Collect()` (读 /proc) | 1-50ms | 否（其他 worker 独立） | 超时后 warn |
| **ProcessThread** | `Dequeue(100ms)` | 100ms | 否（每管道独立线程） | — |
| **ProcessThread** | `RunProcessors()` | ~1-100μs | 否 | 设计约束: Processor 必须快 |
| **ProcessThread** | `Aggregator::Flush()` | ~1-10μs (swap) | 否 | 设计约束: Flush 是 swap |
| **ProcessThread** | `SubmitToSinks()` | ~100ns (入队) | 否 | — |
| **SinkPool Worker** | `Sink::Write()` (HTTP/文件) | 1-5000ms | 否（其他 worker 独立） | 超时+重试 |

**结论**: ProcessThread 在完整事件循环中，最大阻塞 = `Dequeue` 超时 100ms。所有 I/O 操作都在池化线程中执行，不影响处理路径。

## 七、配置设计

### 7.1 YAML 配置

```yaml
engine:
  # TimerWheel 无需配置（固定 1 线程）
  
  collect_pool_threads: 2   # CollectPool 大小 (0=auto: 2)
  sink_pool_threads: 4      # SinkPool 大小 (0=auto: CPU/2)
  
  channel:
    size: medium             # small(1024) | medium(4096) | large(16384)
    drop_policy: drop_newest # drop_newest | drop_oldest
    backpressure_high: 0.8
    backpressure_low: 0.2
```

### 7.2 EngineConfig 结构体

```cpp
struct EngineConfig {
    uint32_t collect_pool_threads = 0;  // 0 = auto (2 threads)
    uint32_t sink_pool_threads = 0;     // 0 = auto (CPU/2)
    
    struct ChannelConfig {
        std::string size = "medium";
        std::string drop_policy = "drop_newest";
        double backpressure_high = 0.8;
        double backpressure_low = 0.2;
    } channel;
};
```

## 八、优雅停机序列

```
FeatureBus::RemoveAll() + InfrastructureManager::Stop()
   │
   ├─ 1. TimerWheel::Stop()
   │     running_ = false → Wakeup(eventfd) → thread_.join()
   │     停止所有定时器，不再触发新的 Collect/Flush 事件
   │
   ├─ 2. CollectPool 销毁 (collect_pool_.reset())
   │     等待正在执行的 Collect() 完成，排空任务队列
   │
   ├─ 3. For each Pipeline: Pipeline::Stop()
   │     │
   │     ├─ 3a. Source::Stop()
   │     │      Push Source 停止回调
   │     │
   │     ├─ 3b. running_ = false
   │     │
   │     ├─ 3c. process_thread_.join()
   │     │      ProcessThread 退出前:
   │     │      - 退出主循环的 while(running_)
   │     │      - 调用 Drain() 排空 channel 中剩余数据
   │     │      - Drain 中执行最后一次 Aggregator::Flush()
   │     │      - SubmitToSinks(最后一批数据)
   │     │
   │     ├─ 3d. Processor/Aggregator/Sink Stop()
   │     │      Sink::Flush() 刷出缓冲
   │     │
   │     └─ 3e. 日志: 打印 channel 统计 (enqueued/dequeued/dropped/flush_injected)
   │
   └─ 4. SinkPool 销毁 (析构函数中)
         等待所有 Write() 完成，确保数据不丢失
```

**关键**: InfrastructureManager::Stop() 在所有 FeatureDriver::Remove() 之后调用，确保 ProcessThread drain 阶段提交的最后一批数据能被 SinkPool 写入。

## 九、类图与文件组织

### 9.1 实际文件结构

```
src/
├── cli/
│   └── main.cc                    # 命令行入口（daemon/collect/top/version/plugins/storage）
├── core/
│   ├── common/
│   │   ├── config.h               # 配置结构体（GlobalConfig, PipelineConfig, EngineConfig）
│   │   ├── logging.h              # 日志工具
│   │   ├── status.h               # Status/StatusOr 错误处理
│   │   └── self_observability.h   # 内部指标（InternalMetrics, ResourceLimiter）
│   ├── config/
│   │   └── yaml_config_loader.h   # YAML 配置解析
│   ├── engine/
│   │   ├── data_batch.h           # 数据模型（Record, StackSample, DataBatch, Arena）
│   │   ├── async_channel.h        # 异步通道（variant<DataBatchPtr, FlushSentinel>, 三级退避）
│   │   ├── timer_wheel.h          # 全局定时调度器（timerfd+epoll+eventfd+最小堆）
│   │   ├── feature_driver.h       # Feature 驱动基类（BuildPipeline + Probe/Remove）
│   │   ├── feature_bus.h           # Feature 注册与生命周期编排
│   │   ├── infrastructure_manager.h # 共享基础设施（TimerWheel + CollectPool + SinkPool）
│   │   └── pipeline.h             # Pipeline 类定义（独立于 FeatureDriver）
│   ├── memory/
│   │   ├── arena.h                # Arena 内存分配器（碰撞指针）
│   │   └── lock_free_queue.h      # 无锁队列
│   └── threading/
│       ├── thread_pool.h          # 通用线程池
│       └── thread_util.h          # 线程工具（SetThreadName）
├── plugin/
│   ├── api/
│   │   ├── plugin_api.h           # Plugin 基类 + C ABI 接口
│   │   ├── source_plugin.h        # Source 插件抽象（Pull/Push 模式）
│   │   ├── processor_plugin.h     # Processor 插件抽象
│   │   ├── aggregator_plugin.h    # Aggregator 插件抽象
│   │   └── sink_plugin.h          # Sink 插件抽象
│   ├── builtin/                   # 内置插件注册
│   └── manager/                   # 插件管理器（注册表、so_loader、wasm_runtime）
├── sources/                       # 数据源插件实现（CPU/内存/IO/网络/调度）
├── processors/                    # 处理器插件实现（过滤/透传/符号化/栈合并）
├── aggregators/                   # 聚合器插件实现（CPU 统计聚合）
├── sinks/                         # 数据出口插件实现（10+ 种）
│   ├── recording_sink/            # 录制 Sink（供 API 录制回放）
├── ebpf/                          # eBPF 探针程序（C 源码）+ 加载器
├── server/                        # HTTP 服务器 + API 路由 + WebSocket 管理
├── storage/                       # 存储后端抽象 + SQLite 实现
└── serialization/                 # JSON 序列化
```

### 9.2 组件依赖图

```
                     InfrastructureManager
                     (TimerWheel + CollectPool + SinkPool)
                          │
                 FeatureBus (注册与生命周期编排)
                      │    │
            FeatureDriver[]
                      │    │
                 Pipeline (BuildPipeline)
                      │    │
               AsyncChannel<ChannelItem>
                      │
                 Source::Collect   Sink::Write
                 
                 
              FeatureBus（编排层）
               │
               └── FeatureDriver（实例层）
                    │
                    └── Pipeline（单管道）
                         ├── SourcePlugin
                         ├── ProcessorPlugin[]
                         ├── AggregatorPlugin
                         └── SinkPlugin[]
                              ├── SseSink（自动注入，SSE 实时推送）
                              └── RecordingSink（自动注入，录制回放）
```

### 9.3 FeatureDriver 状态机

```
  Inactive ──→ Probe() → Active ⇄ Paused
     ↑            │         │        │
     │            │         │        │
     └── Remove() ←─────────┴────────┘

  DriverTier 分级:
  - Tier 1 (kMonitoring): procfs 读取，<0.5% CPU，daemon 启动时自动 Probe
  - Tier 2 (kTracing):    轻量 eBPF + procfs，1-3% CPU，自动 Probe
  - Tier 3 (kProfiling):  高频采样，3-10% CPU，必须手动触发 + 指定 target_pids
```

### 9.4 与设计文档的差异总结

| 维度 | 原设计文档 | 实际实现 |
|------|-----------|---------|
| TimerWheel 实现 | priority_queue + cv::wait_until | timerfd + epoll + eventfd + 最小堆 |
| 取消策略 | 未提及 | 懒惰删除（Lazy Deletion） |
| ProcessThread 出队 | 简单 Dequeue | 三级自适应退避（spin→yield→sleep） |
| Sink 提交 | 永远非阻塞 | 带过载保护（>256 pending 丢弃） |
| 优雅停机 | 基本描述 | 完整 Drain 流程（排空 channel + 最后 flush） |
| 资源检查 | 未提及 | 每 100 次循环检查内存限制 |
| BPF 启动 | 无 | 100ms 延迟避免内核过载 |

## 十、Metrics 自观测

### 新增 / 变更的指标

| 指标名 | 类型 | 含义 |
|--------|------|------|
| `timer_wheel_timers_active` | Gauge | 当前注册的定时器数量 |
| `timer_wheel_fires_total` | Counter | 定时器触发总次数 |
| `collect_pool_pending_tasks` | Gauge | CollectPool 排队中的任务数 |
| `collect_pool_active_workers` | Gauge | 正在执行 Collect 的 worker 数 |
| `pipeline_*_channel_flush_injected` | Counter | FlushSentinel 注入次数 |
| `sink_pool_pending_tasks` | Gauge | SinkPool 排队中的任务数 |

### MetricsSync 定时器

```cpp
timer_.AddRepeating(std::chrono::seconds(10), [this] {
    collect_pool_->Submit([this] {
        for (auto& p : pipelines_) {
            p->SyncChannelMetrics();
        }
    });
});
```

## 十一、与 v2 的对比（实际实现 vs 设计目标）

| 维度 | v2 | v3（实际实现） | 改进 |
|------|-----|--------------|------|
| Pull 采集 | PullScheduler 单线程串行 | CollectPool 并行 | 消除阻塞 |
| flush 触发 | ProcessLoop 内 if-check | TimerWheel → FlushSentinel | 事件驱动 |
| ProcessThread 职责 | 消费+处理+flush检查+metrics | 纯事件处理器 (2 种 event) | 单一职责 |
| Sink 写入 | 单 Sink 同步快路径 | 全部走 SinkPool + 过载保护 | 消除 I/O 阻塞 |
| 定时器管理 | PullScheduler + ProcessLoop 各管各的 | TimerWheel 统一管理 | 架构清晰 |
| channel 类型 | `DataBatchPtr` only | `variant<Data, Sentinel>` | 支持事件多态 |
| 出队策略 | 简单阻塞 | 三级自适应退避（spin→yield→sleep） | 低延迟 + 低 CPU |
| 线程数 (10管道) | ~15 | 1 + 2 + 10 + 4 = 17 | +2 (CollectPool) |
| 最大 I/O 阻塞线程 | ProcessThread (单Sink快路径) | 无 (全部池化) | 完全隔离 |
| 用户层 | 无 | FeatureManager (按需启停/暂停/录制/重配) | 新增 |
| 资源检查 | 无 | 每 100 次循环检查内存限制 | 新增 |
| 优雅停机 | 基本 | 完整 Drain 流程 | 更可靠 |

## 十二、风险与缓解

| 风险 | 概率 | 缓解 |
|------|------|------|
| variant 增大 channel item 内存 | 低 | 从 16B → 24B，4096 容量仅增 32KB |
| CollectPool 任务堆积 | 低 | 监控 pending_tasks，告警阈值 |
| SinkPool 写入超时 | 中 | Sink 插件内实现超时+重试 |
| FlushSentinel 被 drop | 极低 | InjectFlush 优先级保证（可腾出空间） |
| TimerWheel 回调耗时 | 极低 | 回调只做 Submit/Enqueue，微秒级 |
| SinkPool 过载导致数据丢失 | 低 | 过载保护（>256 pending 丢弃），InternalMetrics 可观测 |
| 内存超限 | 中 | ResourceLimiter 每 100 次循环检查，超标时 warn |
| BPF 探针同时挂载导致内核过载 | 低 | 含 BPF 探针的管道启动间 100ms 延迟 |
| 时钟跳变 | 极低 | timerfd 使用 CLOCK_MONOTONIC，不受系统时间调整影响 |

## 十三、实施状态

| 阶段 | 任务 | 状态 |
|------|------|------|
| Phase 1 | 新增 TimerWheel（timerfd+epoll+eventfd） | ✅ 已完成 |
| Phase 2 | AsyncChannel 改为 variant | ✅ 已完成 |
| Phase 3 | Pipeline 重构 ProcessLoop（三级退避出队） | ✅ 已完成 |
| Phase 4 | InfrastructureManager 集成（CollectPool + SinkPool） | ✅ 已完成 |
| Phase 5 | 配置 + 构建 | ✅ 已完成 |
| Phase 6 | FeatureManager 按需启停管理 | ✅ 已完成 |
| Phase 7 | 自动注入 StreamSink + WebSocketSink + RecordingSink | ✅ 已完成 |
| Phase 8 | 优雅停机 Drain 流程 | ✅ 已完成 |
| Phase 9 | 过载保护 + 资源限制检查 | ✅ 已完成 |

**全部实施完成**（2026-05）
