# Illuminator Pipeline v3 — 事件驱动异步管道架构设计

> **版本**: 3.0  
> **状态**: ✅ 已实现（2026-05 完成）  
> **设计理念**: 调度与执行分离 · 单一职责线程 · Actor 式状态隔离  
> **实现文件**: `src/core/engine/pipeline_controller.h/.cc` + `timer_wheel.h`

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
│  │     内部: priority_queue<Event>    │     不执行任何实际工作             │
│  │     唤醒: condition_variable       │                                  │
│  │                                    │                                  │
│  │     注册的事件:                     │                                  │
│  │     ├─ CollectEvent(pipe_A, @1s)   │─── 到时 → 提交 Collect 到 Pool   │
│  │     ├─ CollectEvent(pipe_B, @2s)   │─── 到时 → 提交 Collect 到 Pool   │
│  │     ├─ FlushEvent(pipe_A, @3s)     │─── 到时 → 注入 Sentinel 到 Ch    │
│  │     ├─ FlushEvent(pipe_C, @5s)     │─── 到时 → 注入 Sentinel 到 Ch    │
│  │     └─ MetricsSync(@10s)           │─── 到时 → 提交 Sync 到 Pool      │
│  └────────────────────────────────────┘                                  │
│                     │                                                    │
│            ┌────────┴─────────┐                                          │
│            │                  │                                          │
│            ▼                  ▼                                          │
│  ┌──────────────────┐  ┌──────────────────────────────────────────────┐  │
│  │ CollectPool       │  │ Per Pipeline (× N):                         │  │
│  │ (M threads, 共享)  │  │                                            │  │
│  │                   │  │  ┌────────────────────────────────────────┐ │  │
│  │ 执行:              │  │  │ AsyncChannel<ChannelItem, 4096>       │ │  │
│  │ src.Collect()     │──┼─→│ item = variant<DataBatchPtr, Sentinel> │ │  │
│  │                   │  │  └──────────────────┬─────────────────────┘ │  │
│  │ 结果入队到各管道   │  │                     │                       │  │
│  │ 的 AsyncChannel   │  │                     ▼                       │  │
│  │                   │  │  ┌────────────────────────────────────────┐ │  │
│  └──────────────────┘  │  │ ProcessThread (1 per pipeline)         │ │  │
│                         │  │                                        │ │  │
│  Push Sources:          │  │ 纯事件处理器 (Event Handler):           │ │  │
│  eBPF callback ─────────┼─→│  match item:                          │ │  │
│  → channel.Enqueue()    │  │    DataBatch  → RunProcessors()       │ │  │
│                         │  │               → Aggregator.Add()      │ │  │
│                         │  │               → SubmitToSinks()       │ │  │
│                         │  │    Sentinel   → Aggregator.Flush()    │ │  │
│                         │  │               → SubmitToSinks()       │ │  │
│                         │  │                                        │ │  │
│                         │  │ 保证: 纯 CPU-bound, 不做任何 I/O       │ │  │
│                         │  └────────────────────────┬───────────────┘ │  │
│                         │                           │                  │  │
│                         └───────────────────────────┼──────────────────┘  │
│                                                     │                     │
│  ┌──────────────────────────────────────────────────▼──────────────────┐  │
│  │ SinkPool (K threads, 共享)                                          │  │
│  │ 执行: sink.Write()                                                   │  │
│  │ 职责: 所有 I/O-bound 写入操作                                         │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  线程总计: 1 (TimerWheel) + M (CollectPool) + N (ProcessThread) + K (Sink)│
│  典型 10 管道: 1 + 2 + 10 + 4 = 17 线程                                  │
└──────────────────────────────────────────────────────────────────────────┘
```

## 四、组件详细设计

### 4.1 TimerWheel — 全局定时事件调度器

**职责**: 管理所有周期性事件的触发时机。不执行任何实际工作。

**设计参考**: Fluent Bit 的 `mk_event_loop` + Kafka 的 `TimingWheel`

```cpp
// src/core/engine/timer_wheel.h

namespace illuminator {

class TimerWheel {
public:
    using Callback = std::function<void()>;
    
    struct TimerEntry {
        uint32_t id;
        std::chrono::steady_clock::time_point next_fire;
        std::chrono::milliseconds interval;
        Callback callback;
        bool repeating;
        
        bool operator>(const TimerEntry& o) const { return next_fire > o.next_fire; }
    };

    // 注册周期性定时器，返回 timer_id
    uint32_t AddRepeating(std::chrono::milliseconds interval, Callback cb);
    
    // 注册一次性定时器
    uint32_t AddOnce(std::chrono::milliseconds delay, Callback cb);
    
    // 取消定时器
    void Cancel(uint32_t timer_id);
    
    void Start();  // 启动调度线程
    void Stop();   // 停止并等待退出

private:
    void Run();    // 调度循环
    
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, 
                        std::greater<TimerEntry>> heap_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint32_t next_id_{0};
};

}  // namespace illuminator
```

**调度循环核心逻辑**:

```cpp
void TimerWheel::Run() {
    SetThreadName("il-timer");
    std::unique_lock<std::mutex> lock(mutex_);
    
    while (running_.load(std::memory_order_acquire)) {
        if (heap_.empty()) {
            cv_.wait(lock, [this] { return !running_ || !heap_.empty(); });
            continue;
        }
        
        auto& top = heap_.top();
        if (top.next_fire <= std::chrono::steady_clock::now()) {
            // 到时！取出并执行回调
            auto entry = heap_.top();
            heap_.pop();
            
            lock.unlock();
            entry.callback();   // 回调执行不持锁
            lock.lock();
            
            // 周期性定时器重新入堆
            if (entry.repeating && running_) {
                entry.next_fire += entry.interval;
                heap_.push(std::move(entry));
            }
        } else {
            // 等待到最近的触发时间
            cv_.wait_until(lock, top.next_fire);
        }
    }
}
```

**关键设计决策**:

| 决策 | 选择 | 理由 |
|------|------|------|
| 实现方式 | `timerfd` + `epoll` + `eventfd` | Linux 内核精度高、无忙等待、支持外部唤醒 |
| 回调执行 | 释放锁后执行回调 | 防止回调中注册新定时器导致死锁 |
| 线程数 | 固定 1 线程 | 回调只做 `Submit/Enqueue`，纳秒级完成 |

> **注**: 原设计评审考虑了 `priority_queue + cv::wait_until` 方案（跨平台），
> 最终实现选择了 `timerfd + epoll`（更高精度、支持 `eventfd` 唤醒注册新定时器）。
> 见 `src/core/engine/timer_wheel.h`。

### 4.2 AsyncChannel — 支持 variant 的管道通道

**改造点**: 将 `LockFreeQueue<DataBatchPtr>` 扩展为 `LockFreeQueue<ChannelItem>`，支持数据和 sentinel 两种消息。

```cpp
// src/core/engine/async_channel.h

namespace illuminator {

// FlushSentinel: 无载荷信号，占 1 字节
struct FlushSentinel {};

// ChannelItem: channel 中传输的两种消息类型
using ChannelItem = std::variant<DataBatchPtr, FlushSentinel>;

template <size_t Capacity = 4096>
class AsyncChannel {
public:
    struct Stats {
        std::atomic<uint64_t> enqueued{0};
        std::atomic<uint64_t> dropped{0};
        std::atomic<uint64_t> dequeued{0};
        std::atomic<uint64_t> flush_injected{0};
        std::atomic<uint64_t> backpressure_events{0};
    };

    explicit AsyncChannel(DropPolicy policy = DropPolicy::kDropNewest,
                          double high_wm = 0.8, double low_wm = 0.2);

    // 数据入队（生产端）
    bool TryEnqueue(DataBatchPtr batch);
    
    // Sentinel 注入（TimerWheel 回调）
    // Sentinel 不受 drop 策略影响 — 总是成功入队或替换最旧数据
    bool InjectFlush();
    
    // 消费端：阻塞出队
    std::optional<ChannelItem> Dequeue(std::chrono::milliseconds timeout);
    
    // 消费端：非阻塞出队
    std::optional<ChannelItem> TryDequeue();
    
    bool IsBackpressured() const;
    size_t SizeApprox() const;
    static constexpr size_t capacity() { return Capacity; }
    const Stats& stats() const { return stats_; }

private:
    void UpdateBackpressure();
    
    LockFreeQueue<ChannelItem, Capacity> queue_;
    Stats stats_;
    std::atomic<bool> backpressured_{false};
    DropPolicy drop_policy_;
    double high_wm_, low_wm_;
};
```

**variant 对 LockFreeQueue 的影响分析**:

```
sizeof(DataBatchPtr) = sizeof(shared_ptr<DataBatch>) = 16 bytes
sizeof(FlushSentinel) = 1 byte
sizeof(ChannelItem) = sizeof(variant<16B, 1B>) = 24 bytes (含 discriminant + padding)

每个 Cell = atomic<size_t>(8B) + ChannelItem(24B) = 32 bytes
总内存 = 4096 × 32 = 128 KB (与之前相同量级，可接受)
```

**InjectFlush 的优先级保证**:

```cpp
bool AsyncChannel::InjectFlush() {
    ChannelItem item = FlushSentinel{};
    if (queue_.TryPush(std::move(item))) {
        stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // 队列满时，丢弃一个旧数据腾出空间给 Sentinel
    // flush 事件优先级高于普通数据
    ChannelItem discarded;
    if (queue_.TryPop(discarded)) {
        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    if (queue_.TryPush(std::move(item))) {
        stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
```

### 4.3 ProcessThread — 纯事件处理器

**职责**: 从 channel 消费事件，串行执行有状态逻辑（Processor 链 + Aggregator），提交结果到 SinkPool。

**核心约束**: **不做任何 I/O，不持有任何定时器，不做任何调度决策。**

```cpp
// Pipeline::ProcessLoop (in pipeline_controller.h)

void ProcessLoop() {
    while (running_.load(std::memory_order_acquire)) {
        auto item = ingest_channel_.Dequeue(std::chrono::milliseconds(100));
        if (!item) continue;
        
        std::visit(Overloaded{
            [this](DataBatchPtr& batch) {
                HandleData(std::move(batch));
            },
            [this](FlushSentinel&) {
                HandleFlush();
            },
        }, *item);
    }
    
    Drain();
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

// Sink 提交：永远非阻塞
void SubmitToSinks(DataBatchPtr batch) {
    if (!sink_pool_) {
        // 无池回退：直接写（仅单 Sink 场景可接受）
        for (auto& sink : sinks_) sink->Write(batch);
        return;
    }
    for (auto& sink : sinks_) {
        sink_pool_->Submit([sink = sink.get(), batch] {
            auto status = sink->Write(batch);
            if (!status.ok()) {
                IL_WARN("Sink write error: {}", status.message());
            }
        });
    }
}
```

**Overloaded 辅助**（C++17 标准技巧）:

```cpp
template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;
```

### 4.4 CollectPool — 共享 I/O 工作池

**职责**: 并行执行所有 `Source::Collect()` 调用。

**为什么不复用 SinkPool?** 
- 隔离性：一个 Sink 写入超时不应影响 Source 采集
- 可观测性：分开统计 Collect 和 Write 的排队/执行时间
- 配置独立：Collect 通常需要 2-3 线程，Sink 可能需要 4-8 线程

**实现**: 直接复用现有 `ThreadPool`，与 SinkPool 是两个独立实例。

```cpp
// PipelineController 中:
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
    │──── timer fires ─┐                 │                  │                │
    │                  │                 │                  │                │
    │  Submit(Collect) │                 │                  │                │
    │─────────────────→│                 │                  │                │
    │                  │                 │                  │                │
    │                  │ src.Collect()   │                  │                │
    │                  │───────┐         │                  │                │
    │                  │       │ /proc   │                  │                │
    │                  │←──────┘         │                  │                │
    │                  │                 │                  │                │
    │                  │ TryEnqueue(Data)│                  │                │
    │                  │────────────────→│                  │                │
    │                  │                 │                  │                │
    │                  │                 │  Dequeue(Data)   │                │
    │                  │                 │─────────────────→│                │
    │                  │                 │                  │                │
    │                  │                 │                  │ RunProcessors()│
    │                  │                 │                  │───────┐        │
    │                  │                 │                  │←──────┘        │
    │                  │                 │                  │                │
    │                  │                 │                  │ agg.Add()      │
    │                  │                 │                  │───────┐        │
    │                  │                 │                  │←──────┘        │
    │                  │                 │                  │                │
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
1. PipelineController::StopAll()
   │
   ├─ 2. TimerWheel::Stop()
   │     停止所有定时器，不再触发新的 Collect/Flush 事件
   │
   ├─ 3. CollectPool::~ThreadPool()
   │     等待正在执行的 Collect() 完成，排空任务队列
   │
   ├─ 4. For each Pipeline: Pipeline::Stop()
   │     │
   │     ├─ 4a. Source::Stop()
   │     │      Push Source 停止回调
   │     │
   │     ├─ 4b. running_ = false
   │     │
   │     ├─ 4c. ProcessThread join
   │     │      ProcessThread 退出前: 
   │     │      - Drain channel 中剩余数据
   │     │      - 执行最后一次 Aggregator::Flush()
   │     │      - SubmitToSinks(最后一批数据)
   │     │
   │     ├─ 4d. Processor/Aggregator/Sink Stop()
   │     │      Sink::Flush() 刷出缓冲
   │     │
   │     └─ 4e. 日志: 打印 channel 统计 (enqueued/dequeued/dropped)
   │
   └─ 5. SinkPool::~ThreadPool()
         等待所有 Write() 完成，确保数据不丢失
```

**关键**: SinkPool 最后销毁，确保 ProcessThread drain 阶段提交的最后一批数据能被写入。

## 九、类图与文件组织

### 9.1 新增/修改的文件

```
src/core/engine/
├── timer_wheel.h           [新增] TimerWheel 定义与实现
├── async_channel.h         [修改] 支持 variant<DataBatchPtr, FlushSentinel>
├── pipeline_controller.h   [修改] Pipeline + PipelineController 重构
└── pipeline_controller.cc  [修改] BuildFromConfig, StartAll, StopAll

src/core/common/
└── config.h                [修改] EngineConfig 新增 collect_pool_threads

src/core/config/
└── yaml_config_loader.h    [修改] 解析 collect_pool_threads

src/core/BUILD              [修改] 新增 timer_wheel.h
```

### 9.2 删除的组件

| 组件 | 原位置 | 替代方案 |
|------|--------|---------|
| `PullScheduler` | pipeline_controller.h | `TimerWheel` + `CollectPool` |
| ProcessLoop 内 flush if-check | pipeline_controller.h | `FlushSentinel` 事件驱动 |
| 单 Sink 快路径 | Pipeline::DeliverToSinks | 统一走 SinkPool |

### 9.3 组件依赖图

```
                     GlobalConfig
                          │
                 PipelineController
                    │           │
              TimerWheel     Pipelines[]
                    │           │
             ┌──────┤      Pipeline
             │      │        │    │
      CollectPool   │   ProcessThread
             │      │        │    │
             │      │  AsyncChannel<ChannelItem>
             │      │        │
             │      └── SinkPool
             │              │
         Source::Collect   Sink::Write
```

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

## 十一、与当前实现 (v2) 的对比

| 维度 | v2 (当前) | v3 (本设计) | 改进 |
|------|----------|------------|------|
| Pull 采集 | PullScheduler 单线程串行 | CollectPool 并行 | 消除阻塞 |
| flush 触发 | ProcessLoop 内 if-check | TimerWheel → FlushSentinel | 事件驱动 |
| ProcessThread 职责 | 消费+处理+flush检查+metrics | 纯事件处理器 (2 种 event) | 单一职责 |
| Sink 写入 | 单 Sink 同步快路径 | 全部走 SinkPool | 消除 I/O 阻塞 |
| 定时器管理 | PullScheduler + ProcessLoop 各管各的 | TimerWheel 统一管理 | 架构清晰 |
| channel 类型 | `DataBatchPtr` only | `variant<Data, Sentinel>` | 支持事件多态 |
| 线程数 (10管道) | 15 | 17 | +2 (CollectPool) |
| 最大 I/O 阻塞线程 | ProcessThread (单Sink快路径) | 无 (全部池化) | 完全隔离 |

## 十二、风险与缓解

| 风险 | 概率 | 缓解 |
|------|------|------|
| variant 增大 channel item 内存 | 低 | 从 16B → 24B，4096 容量仅增 32KB |
| CollectPool 任务堆积 | 低 | 监控 pending_tasks，告警阈值 |
| SinkPool 写入超时 | 中 | Sink 插件内实现超时+重试 |
| FlushSentinel 被 drop | 极低 | InjectFlush 优先级保证（可腾出空间） |
| TimerWheel 回调耗时 | 极低 | 回调只做 Submit/Enqueue，微秒级 |

## 十三、实施计划

| 阶段 | 任务 | 预计改动 |
|------|------|---------|
| **Phase 1** | 新增 TimerWheel | +1 新文件 |
| **Phase 2** | AsyncChannel 改为 variant | 修改 1 文件 |
| **Phase 3** | Pipeline 重构 ProcessLoop | 修改 1 文件 |
| **Phase 4** | PipelineController 集成 | 修改 2 文件 |
| **Phase 5** | 配置 + 构建 | 修改 3 文件 |
| **Phase 6** | 编译验证 + 运行测试 | — |

总改动量: ~300 行新增, ~200 行删除, ~100 行修改
