# FeatureDriver v2 设计文档

> 临时设计文档，用于讨论和评审。最终确认后合并到正式文档。

---

## 一、Reconfigure 分发改造

### 1.1 问题

当前 `Reconfigure` 仅转发给 Source：

```cpp
// 当前实现 — 所有 Driver 的子类都是这样写的
Status Reconfigure(const ConfigValue& params) override {
    if (!pipeline_ || !pipeline_->GetSource()) {
        return Status::Error(StatusCode::kUnavailable, "not running");
    }
    return pipeline_->GetSource()->Reconfigure(params);
}
```

**缺陷**：Pipeline 的其他阶段（Processor、Aggregator、Sink）也可能需要运行时调参。例如：
- `StackSymbolizerProcessor`：运行时开关 `demangle`、`kernel_symbols`
- `Aggregator`：运行时修改 `flush_interval_ms`
- `SseSink`：运行时修改 `feature_name`

### 1.2 设计：Driver 自主分发

参考 Linux Driver 的 `ioctl` 模型，Driver 作为"指挥官"，自己决定参数如何分发。

#### 基类 FeatureDriver 默认实现

```cpp
// feature_driver.h
class FeatureDriver {
public:
    // Reconfigure — 运行时动态更新 Feature 参数
    // Driver 自主决定哪些参数分发给哪个 Pipeline 阶段。
    // 子类可以完全覆盖，也可以调用基类默认实现。
    virtual Status Reconfigure(const ConfigValue& params) {
        if (!pipeline_) {
            return Status::Error(StatusCode::kUnavailable, "pipeline not running");
        }

        Status last_error;

        // 1. 分发给 Source
        if (auto* src = pipeline_->GetSource()) {
            auto st = src->Reconfigure(params);
            if (!st.ok()) last_error = st;
        }

        // 2. 分发给所有 Processor
        for (auto& proc : pipeline_->GetProcessors()) {
            auto st = proc->Reconfigure(params);
            if (!st.ok()) last_error = st;
        }

        // 3. 分发给 Aggregator
        if (auto* agg = pipeline_->GetAggregator()) {
            auto st = agg->Reconfigure(params);
            if (!st.ok()) last_error = st;
        }

        // 4. 分发给所有 Sink
        for (auto& sink : pipeline_->GetSinks()) {
            auto st = sink->Reconfigure(params);
            if (!st.ok()) last_error = st;
        }

        return last_error;
    }

    // 子类可以覆盖，加入 Driver 自身参数的更新逻辑：
    // Status Reconfigure(const ConfigValue& params) override {
    //     // 1. 更新 Driver 自身状态
    //     if (params.Has("interval_ms")) {
    //         interval_ms_ = params["interval_ms"].AsInt(interval_ms_);
    //         // Pull 模式需要重建 Timer
    //         if (state_ == DriverState::kActive &&
    //             !pipeline_->GetSource()->IsPushMode()) {
    //             UnregisterTimers(InfrastructureManager::Instance());
    //             RegisterTimers(InfrastructureManager::Instance());
    //         }
    //     }
    //     // 2. 调用基类默认分发
    //     return FeatureDriver::Reconfigure(params);
    // }
};
```

#### 各插件基类新增 Reconfigure 虚方法

```cpp
// plugin/api/processor_plugin.h
class ProcessorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kProcessor; }
    virtual StatusOr<DataBatchPtr> Process(DataBatchPtr batch) = 0;

    // 新增：运行时重配置（默认不支持）
    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Ok();  // 默认成功，大多数 Processor 不需要
    }
};

// plugin/api/aggregator_plugin.h
class AggregatorPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kAggregator; }
    virtual Status Add(DataBatchPtr batch) = 0;
    virtual StatusOr<std::vector<DataBatchPtr>> Flush() = 0;
    virtual uint32_t FlushIntervalMs() const = 0;

    // 新增：运行时重配置
    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Ok();
    }
};

// plugin/api/sink_plugin.h
class SinkPlugin : public Plugin {
public:
    PluginType Type() const override { return PluginType::kSink; }
    virtual Status Write(DataBatchPtr batch) = 0;
    virtual Status Flush() { return Status::Ok(); }

    // 新增：运行时重配置
    virtual Status Reconfigure(const ConfigValue& /*params*/) {
        return Status::Ok();
    }
};
```

#### Pipeline 新增访问器

```cpp
// pipeline.h 新增方法
class Pipeline {
public:
    // ... 现有接口 ...

    const std::vector<std::unique_ptr<ProcessorPlugin>>& GetProcessors() const {
        return processors_;
    }
    AggregatorPlugin* GetAggregator() { return aggregator_.get(); }
    const std::vector<std::unique_ptr<SinkPlugin>>& GetSinks() const {
        return sinks_;
    }

    // 运行时动态添加 Sink（线程安全）
    Status AddSinkRuntime(std::unique_ptr<SinkPlugin> sink) {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sinks_.push_back(std::move(sink));
        return Status::Ok();
    }

private:
    mutable std::mutex sink_mutex_;  // 保护 sinks_ 动态修改
};
```

#### 具体 Driver 子类改写示例

```cpp
// cpu_utilization_driver.h
class CpuUtilizationDriver : public FeatureDriver {
    // ... 省略元数据方法 ...

    Status Reconfigure(const ConfigValue& params) override {
        // 1. 更新 Driver 自身参数
        if (params.Has("interval_ms")) {
            interval_ms_ = static_cast<uint32_t>(params["interval_ms"].AsInt(1000));
            if (state_ == DriverState::kActive) {
                UnregisterTimers(InfrastructureManager::Instance());
                RegisterTimers(InfrastructureManager::Instance());
            }
        }
        if (params.Has("collect_per_core")) {
            collect_per_core_ = params["collect_per_core"].AsBool(true);
        }
        if (params.Has("ema_alpha")) {
            ema_alpha_ = params["ema_alpha"].AsDouble(0.0);
        }

        // 2. 分发给 Pipeline 各阶段
        return FeatureDriver::Reconfigure(params);
    }
};
```

---

## 二、定时器驱动的统一采集模型

### 2.1 所有 Source 统一为 Pull 模式

深入分析后，**所有 Source 都可以用 TimerWheel 驱动**，不需要专用线程：

| 数据源类型 | 消费方式 | TimerWheel 间隔 | 例子 |
|-----------|---------|:---:|------|
| 文件系统（/proc、/sys） | 同步 read | 1000ms | CpuUtilization |
| BPF maps（HASH/ARRAY） | 同步 syscall | 1000ms | CpuProfiler aggregated, OffcpuProfiler |
| ring buffer（实时事件） | 非阻塞 poll(0) | 100ms | CpuProfiler stream |

**关键突破**：`ring_buffer__poll(ring_buf_, 0)` 是非阻塞的——立即排空所有积压事件后返回，不阻塞。这意味着可以在 `Collect()` 中调用，不需要专用 poll 线程。

### 2.2 统一架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                    统一 Pull 模式（TimerWheel 驱动）                  │
│                                                                      │
│  TimerWheel → CollectPool → Collect() → 同步读数据源 → 返回 batch     │
│                                                                      │
│  数据源：                                                             │
│  ├─ /proc、/sys 文件系统：同步 read()                                 │
│  ├─ BPF maps（HASH/ARRAY）：bpf_map_lookup_elem() + get_next_key()   │
│  └─ ring buffer：ring_buffer__poll(0) 非阻塞排空                      │
│                                                                      │
│  暂停：UnregisterTimers()  — 取消 TimerWheel entry                   │
│  恢复：RegisterTimers()    — 重新注册 TimerWheel entry                │
│                                                                      │
│  无额外线程，无 push/poll 线程，架构完全一致                           │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.3 各插件重构

#### CpuProfilerSource stream 模式（原 Push 模式）

```cpp
// 改造后：去掉 poll_thread_、callback_、SetCallback()
// 改为非阻塞 poll 模式

bool IsPushMode() const override { return false; }  // 统一 Pull
uint32_t IntervalMs() const override { return 100; }  // 每 100ms poll 一次

StatusOr<DataBatchPtr> Collect() override {
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    pending_batch_ = batch.get();

    // 非阻塞排空 ring buffer 中所有积压事件
    // ring_buffer__poll(0) 立即返回，每收到一个事件 → HandleStreamEvent 填充 pending_batch_
    while (ring_buffer__poll(ring_buf_, 0) > 0) {}

    pending_batch_ = nullptr;
    return batch;
}

private:
    DataBatch* pending_batch_ = nullptr;  // 期间回调累积目标

    static int HandleStreamEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<CpuProfilerSource*>(ctx);
        if (!self->pending_batch_ || size < sizeof(il_cpu_sample_event))
            return 0;
        auto* ev = static_cast<il_cpu_sample_event*>(data);
        auto& sample = self->pending_batch_->AddStackSample();
        // ... 填充 sample 字段 ...
        return 0;
    }
```

**性能分析**：16 核 × 49Hz = 784 事件/秒，100ms 间隔 ≈ 78 事件/批。ring buffer 4MB+ 可容纳数万事件，100ms 不会溢出。

#### CpuProfilerSource aggregated 模式

```cpp
// 去掉 agg_thread_、AggregatedPullLoop()
// Collect() 直接同步读 BPF maps

bool IsPushMode() const override { return false; }
uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

StatusOr<DataBatchPtr> Collect() override {
    if (counts_fd_ < 0 || stacks_fd_ < 0)
        return Status::Error(StatusCode::kUnavailable, "BPF not loaded");
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    SnapshotAndClearCounts(batch.get());  // 原子：读 + 清 HASH map
    return batch;
}
```

#### OffcpuProfilerSource

```cpp
// 去掉 poll_thread_、ring_buf_、HandleEvent
// 去掉 BPF 侧 ring buffer 代码
// Collect() 直接同步读 BPF maps

bool IsPushMode() const override { return false; }
uint32_t IntervalMs() const override { return 1000; }

StatusOr<DataBatchPtr> Collect() override {
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    ReadAndClearStats(batch.get());  // 同步读 HASH map
    return batch;
}
```

### 2.4 暂停/恢复彻底统一

所有插件统一为 Pull 模式后，暂停/恢复变得极其简单：

```cpp
// feature_driver.h
Status Pause() {
    if (state_ != DriverState::kActive) { ... }
    UnregisterTimers(InfrastructureManager::Instance());  // 一行搞定
    state_ = DriverState::kPaused;
    return Status::Ok();
}

Status Resume() {
    if (state_ != DriverState::kPaused) { ... }
    RegisterTimers(InfrastructureManager::Instance());  // 一行搞定
    state_ = DriverState::kActive;
    return Status::Ok();
}
```

**不再需要 `PauseDataFlow()`、`ResumeDataFlow()`、`flow_paused_`、`IsPushMode()` 等 Push 模式专属机制。**

### 2.5 性能对比

| 指标 | 改造前（专用线程） | 改造后（TimerWheel） |
|------|------|------|
| 额外线程 | CpuProfiler: 1-2, Offcpu: 1 | **0** |
| 空闲时 CPU | 线程持续 epoll 唤醒 | **零开销** |
| 暂停实现 | 各自实现 + flow_paused_ | **统一 UnregisterTimers()** |
| 架构一致性 | 3 种模式，不一致 | **1 种模式，完全一致** |
| 数据延迟 | stream: ~0ms, aggregated: 1s | stream: ≤100ms, aggregated: 1s |
| 批量处理 | stream: 1 事件/次 | stream: ~78 事件/次（**减少 Pipeline 开销**） |

### 2.6 可精简的接口

统一 Pull 后，以下接口可以移除：

| 接口 | 原用途 | 移除原因 |
|------|-------|---------|
| `SourcePlugin::SetCallback()` | Push 模式设置回调 | 不再需要 |
| `SourcePlugin::IsPushMode()` | 标识 Push 模式 | 始终 false |
| `SourcePlugin::PauseDataFlow()` | Push 暂停 | TimerWheel 统一处理 |
| `SourcePlugin::ResumeDataFlow()` | Push 恢复 | TimerWheel 统一处理 |
| `SourcePlugin::flow_paused_` | 原子标志 | 不再需要 |
| `Pipeline::Start()` 中 SetCallback 逻辑 | Push 回调注册 | 不再需要 |

---

## 三、eBPF 数据采集的数据结构选择

### 3.1 BPF maps vs ring buffer：本质区别

#### BPF maps：共享内存键值存储

BPF maps 是 BPF 程序和用户态之间**共享的、有结构的内核内存区域**。本质上是**键值存储**。

```
用户态 (同步 syscall)               内核态
┌──────────────────┐              ┌──────────────────────┐
│ bpf_map_lookup_  │   syscall    │  BPF maps (内核内存)   │
│ elem()           │◄────────────►│  ┌─────────────────┐ │
│ bpf_map_update_  │              │  │ HASH map:        │ │
│ elem()           │              │  │  key → val       │ │
│ bpf_map_get_next │              │  │  key → val       │ │
│ _key()           │              │  │   ...            │ │
│                  │              │  └─────────────────┘ │
│                  │              │  BPF 程序:            │
│                  │              │  bpf_map_update_elem │
│                  │              │  bpf_map_lookup_elem │
└──────────────────┘              └──────────────────────┘
```

**特点**：
- 数据**持久存储**，读取不消费
- 读取是**同步 syscall**（用户态主动发起，调用即返回）
- 支持多种类型：HASH、ARRAY、PERCPU_HASH、LRU_HASH 等
- 适合**聚合数据**（计数、统计、配置）
- **不需要 poll 线程**

#### ring buffer：无锁环形队列

ring buffer（`BPF_MAP_TYPE_RINGBUF`）是一个**环形队列**，BPF 程序写入，用户态必须持续 poll 消费。

```
用户态 (必须持续 poll)             内核态
┌──────────────────┐           ┌──────────────────────────┐
│ ring_buffer_poll │  epoll    │  ring buffer              │
│ () ◄─────────────┼───────────┤  ┌────┬────┬────┬──────┐ │
│                  │  事件通知  │  │ ev │ ev │ ev │ ...  │ │
│ callback()       │           │  └────┴────┴────┴──────┘ │
│                  │           │    ↑                ↑     │
│                  │           │  reserve          submit  │
│                  │           │                          │
│                  │           │  BPF 程序:                │
│                  │           │  bpf_ringbuf_reserve     │
│                  │           │  bpf_ringbuf_submit      │
└──────────────────┘           └──────────────────────────┘
```

**特点**：
- 数据是**流式消费**的，消费后空间被回收
- 用户态**必须持续 poll**（基于 epoll），不能"随时读"
- 只有这一种类型（环形队列）
- 适合**实时事件流**
- **必须有 poll 线程**

#### 结论：数据在哪，消费方式就定在哪

| 数据在哪 | 消费方式 | 需要 poll 线程？ | 例子 |
|---------|---------|:---:|------|
| BPF maps（HASH/ARRAY） | 同步 syscall 读取 | **否** | 聚合计数、堆栈数据、配置 |
| ring buffer | 必须持续 poll | **是** | 实时事件流 |
| /proc、/sys 文件系统 | 同步 read | **否** | CPU 利用率、内存统计 |

### 3.2 当前项目中的冗余后台线程

#### 冗余一：CpuProfilerSource aggregated 模式

```cpp
// 当前：有一个专用的 agg_thread_ 做 sleep + 读 BPF maps
agg_thread_ = std::thread([this] {
    SetThreadName("cpuprofiler-agg");
    AggregatedPullLoop();  // sleep(interval) → FlushAggregatedCounts()
});
```

数据在 `stack_counts`（HASH map）和 `stacks`（STACK_TRACE map）中。**完全可以用 TimerWheel 驱动同步读取**，不需要专用线程。

#### 冗余二：OffcpuProfilerSource

```cpp
// 当前：poll 线程等 ring buffer 信号，信号触发后再读 BPF maps
poll_thread_ = std::thread([this] {
    while (running_.load()) {
        ring_buffer__poll(ring_buf_, 100);  // 等信号
    }
});
// 信号回调 (pid=0 event) → ReadAndClearStats() → 读 HASH map
```

数据在 `offcpu_stats`（HASH map）和 `offcpu_stacks`（STACK_TRACE map）中。ring buffer 只用来发一个**通知信号**（pid=0 event），告诉用户态"有数据了来读"。**TimerWheel 的定时调度完全可以替代这个通知**。

### 3.3 统一架构：TimerWheel 驱动所有 Source

#### 重构后：CpuProfilerSource stream 模式（原 Push 模式）

```cpp
// 去掉 poll_thread_、callback_、SetCallback()
// 改为非阻塞 poll 模式

bool IsPushMode() const override { return false; }  // 统一 Pull
uint32_t IntervalMs() const override { return 100; }  // 每 100ms poll 一次

StatusOr<DataBatchPtr> Collect() override {
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    pending_batch_ = batch.get();

    // 非阻塞排空 ring buffer 中所有积压事件
    while (ring_buffer__poll(ring_buf_, 0) > 0) {}

    pending_batch_ = nullptr;
    return batch;
}

private:
    DataBatch* pending_batch_ = nullptr;  // 期间回调累积目标
```

#### 重构后：CpuProfilerSource aggregated 模式

```cpp
// 去掉 agg_thread_、AggregatedPullLoop()
// Collect() 直接同步读 BPF maps

bool IsPushMode() const override { return false; }  // 统一 Pull
uint32_t IntervalMs() const override { return aggregate_interval_ms_; }

StatusOr<DataBatchPtr> Collect() override {
    if (counts_fd_ < 0 || stacks_fd_ < 0)
        return Status::Error(StatusCode::kUnavailable, "BPF not loaded");
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    SnapshotAndClearCounts(batch.get());  // 原子：读 + 清 HASH map
    return batch;
}
```

#### 重构后：OffcpuProfilerSource

```cpp
// 去掉 poll_thread_、ring_buf_、HandleEvent
// 去掉 BPF 侧 ring buffer 代码
// Collect() 直接同步读 BPF maps

bool IsPushMode() const override { return false; }  // 统一 Pull
uint32_t IntervalMs() const override { return 1000; }

StatusOr<DataBatchPtr> Collect() override {
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    ReadAndClearStats(batch.get());  // 同步读 HASH map
    return batch;
}
```

#### 性能对比

| 指标 | 重构前（专用线程） | 重构后（TimerWheel） |
|------|------|------|
| 额外线程 | CpuProfiler: 1-2, Offcpu: 1 | **0** |
| 空闲时 CPU | 线程持续 epoll 唤醒 | **零开销** |
| 暂停实现 | 各自实现 + flow_paused_ | **统一 UnregisterTimers()** |
| 架构一致性 | 3 种模式，不一致 | **1 种模式，完全一致** |
| 数据延迟 | stream: ~0ms, aggregated: 1s | stream: ≤100ms, aggregated: 1s |
| 批量处理 | stream: 1 事件/次 | stream: ~78 事件/次（**减少 Pipeline 开销**） |

### 3.4 暂停/恢复彻底统一

所有插件统一为 Pull 模式后，暂停/恢复变得极其简单：

```cpp
// feature_driver.h
Status Pause() {
    if (state_ != DriverState::kActive) { ... }
    UnregisterTimers(InfrastructureManager::Instance());  // 一行搞定
    state_ = DriverState::kPaused;
    return Status::Ok();
}

Status Resume() {
    if (state_ != DriverState::kPaused) { ... }
    RegisterTimers(InfrastructureManager::Instance());  // 一行搞定
    state_ = DriverState::kActive;
    return Status::Ok();
}
```

**不再需要 `PauseDataFlow()`、`ResumeDataFlow()`、`flow_paused_`、`IsPushMode()` 等 Push 模式专属机制。**

### 3.5 内核级暂停（可选性能优化）

统一 TimerWheel 后，`UnregisterTimers()` 已经足够实现暂停（停止 `Collect()` 调用）。但 **eBPF 程序本身仍在内核中运行**，持续消耗 CPU。内核级暂停是额外的性能优化，对高频 tracepoint 插件收益最大。

#### 设计：SourcePlugin 可选钩子

```cpp
// plugin/api/source_plugin.h
class SourcePlugin : public Plugin {
public:
    // 可选：内核级暂停（性能优化，非必需）
    // 默认空实现：TimerWheel 暂停已足够
    virtual Status PauseKernel() { return Status::Ok(); }
    virtual Status ResumeKernel() { return Status::Ok(); }
};
```

```cpp
// feature_driver.h
Status Pause() {
    if (state_ != DriverState::kActive) { ... }
    UnregisterTimers(InfrastructureManager::Instance());  // 停止 Collect()
    if (pipeline_ && pipeline_->GetSource()) {
        pipeline_->GetSource()->PauseKernel();  // 可选：停止内核数据产生
    }
    state_ = DriverState::kPaused;
    return Status::Ok();
}

Status Resume() {
    if (state_ != DriverState::kPaused) { ... }
    if (pipeline_ && pipeline_->GetSource()) {
        pipeline_->GetSource()->ResumeKernel();  // 可选：恢复内核数据产生
    }
    RegisterTimers(InfrastructureManager::Instance());  // 恢复 Collect()
    state_ = DriverState::kActive;
    return Status::Ok();
}
```

#### 各插件实现

**CpuProfilerSource（perf_event 驱动）**：

```cpp
Status PauseKernel() override {
    for (int fd : perf_fds_) {
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);  // 内核停止产生采样事件
    }
    IL_INFO("cpu_profiler: kernel paused ({} perf fds disabled)", perf_fds_.size());
    return Status::Ok();
}

Status ResumeKernel() override {
    for (int fd : perf_fds_) {
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);   // 重置计数器
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);  // 重新启用
    }
    IL_INFO("cpu_profiler: kernel resumed");
    return Status::Ok();
}
```

**OffcpuProfilerSource（tracepoint 驱动，利用已有机制）**：

```cpp
Status PauseKernel() override {
    if (cfg_fd_ >= 0) {
        uint32_t k0 = 0;
        bpf_map_update_elem(cfg_fd_, &k0, &cfg_flags_base_, BPF_ANY);  // 清除 bit4
    }
    IL_INFO("offcpu_profiler: kernel paused (BPF cfg flag cleared)");
    return Status::Ok();
}

Status ResumeKernel() override {
    if (cfg_fd_ >= 0) {
        uint32_t k0 = 0;
        uint32_t enabled = cfg_flags_base_ | 16u;  // 设置 bit4
        bpf_map_update_elem(cfg_fd_, &k0, &enabled, BPF_ANY);
    }
    IL_INFO("offcpu_profiler: kernel resumed");
    return Status::Ok();
}
```

#### 性能收益

| 插件 | 暂停时 BPF 触发频率 | 暂停前每事件开销 | 暂停后每事件开销 | 每秒节省 |
|------|:---:|:---:|:---:|:---:|
| CpuProfiler | 16 核 × 49Hz = 784/s | 完整采集路径 | 0（内核不触发） | 784 × 采集路径 |
| OffcpuProfiler | 数千次调度/s | 完整采集路径 | ~50ns（map lookup + return） | 数千 × 采集路径 |
| CpuUtilization | 无内核代码 | 0 | 0 | 0 |

> **注**：`PauseKernel()` 是**可选优化**。如果不实现，`UnregisterTimers()` 已经足够——只是 eBPF 程序仍在运行。`PauseKernel()` 消除这个"浪费"。

### 3.6 BPF 内核级暂停方案对比

#### 当前主流方案

| 方案 | 暂停机制 | 暂停延迟 | 暂停时 CPU | 数据安全 | 内核要求 | 适用插件 |
|------|---------|:---:|:---:|------|:---:|------|
| **perf_event IOC_DISABLE** | ioctl 禁用内核事件 | ~1μs | **零** | 保留在 maps/ring buf | 4.x | CpuProfiler |
| **BPF map 配置标志** | 写 map 标记 | ~1μs | 极低（BPF 入口检查） | 保留在 maps | 4.x | OffcpuProfiler |
| **bpf_link detach** | 分离 tracepoint 钩子 | 毫秒级 | **零** | 保留在 maps | 5.7+ | OffcpuProfiler |
| **BPF 卸载/重装** | 完全卸载程序 | 数百毫秒 | **零** | **maps 全丢** | 4.x | 不推荐 |

#### 未来优化方向（按优先级）

**方向一：BPF skeleton + 全局变量（内核 ≥ 5.5）**

当前 OffcpuProfiler 做法：每次 BPF 入口都要 `bpf_map_lookup_elem(&offcpu_cfg, &k0)` 查 HASH map，开销 ~50ns。

改用 BPF skeleton 全局变量后：

```c
// BPF 侧 — 直接读全局变量，零开销
// offcpu_profiler.bpf.c
volatile const uint32_t cfg_flags = 0;  // RODATA 段，用户态可写

SEC("tracepoint/sched/sched_switch")
int trace_offcpu(void *ctx) {
    if (!(cfg_flags & 16))  // 直接内存读取，~1ns，无 map lookup
        return 0;
    // ... 正常采集逻辑 ...
}
```

```cpp
// 用户态 — 直接写 mmap 内存，无 syscall
skel->rodata->cfg_flags = cfg_flags_base_ | 16u;  // mmap 直接写入，~100ns
```

| 指标 | BPF map（当前） | BPF skeleton 全局变量 |
|------|:---:|:---:|
| BPF 入口检查开销 | ~50ns（map lookup） | **~1ns**（直接内存读） |
| 用户态写入开销 | ~1μs（syscall） | **~100ns**（mmap write） |
| 内核版本要求 | 4.x+ | 5.5+（BPF skeleton） |

**方向二：bpf_link 生命周期管理（libbpf ≥ 1.0 + 内核 ≥ 5.7）**

对于 tracepoint 驱动的 BPF 程序，可以完全分离/重新附加：

```cpp
Status PauseKernel() {
    bpf_link__detach(offcpu_link_);  // 分离 → tracepoint 不再触发 BPF
    return Status::Ok();
}

Status ResumeKernel() {
    bpf_link__attach(offcpu_link_);  // 重新附加 → 恢复触发
    return Status::Ok();
}
```

**优势**：真正的零开销暂停（tracepoint 完全不触发 BPF 程序）。**代价**：需要较新内核和 libbpf。

**方向三：PERCPU_ARRAY 替代 HASH（中等优化）**

`offcpu_cfg` 改为 PERCPU_ARRAY 类型，lookup 无哈希计算，~10-20ns：

```c
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, uint32_t);
    __type(value, uint32_t);
} offcpu_cfg SEC(".maps");
```

| 指标 | HASH map | PERCPU_ARRAY |
|------|:---:|:---:|
| lookup 开销 | ~30-50ns | **~10-20ns** |
| 多核并发 | 有缓存行跳动 | 无争用（per-CPU） |

#### 推荐实施路径

```
Phase 1（立即实施）：PauseKernel/ResumeKernel 可选钩子
  ├─ CpuProfiler: PERF_EVENT_IOC_DISABLE/ENABLE
  │   └─ 5 行代码，最大收益，零风险
  └─ OffcpuProfiler: 利用已有的 BPF map flag
      └─ 0 行代码，已有机制

Phase 2（后续优化，内核 ≥ 5.5）：
  └─ 迁移到 BPF skeleton + 全局变量
     └─ 从 ~50ns 降到 ~1ns per event

Phase 3（未来，libbpf ≥ 1.0 + 内核 ≥ 5.7）：
  └─ bpf_link detach/attach
     └─ 真正的零开销暂停
```

### 3.7 可精简的接口

统一 Pull 后，以下接口可以移除：

| 接口 | 原用途 | 移除原因 |
|------|-------|---------|
| `SourcePlugin::SetCallback()` | Push 模式设置回调 | 不再需要 |
| `SourcePlugin::IsPushMode()` | 标识 Push 模式 | 始终 false |
| `SourcePlugin::PauseDataFlow()` | Push 暂停（如果有） | TimerWheel 统一处理 |
| `SourcePlugin::ResumeDataFlow()` | Push 恢复（如果有） | TimerWheel 统一处理 |
| `SourcePlugin::flow_paused_` | 原子标志（如果有） | 不再需要 |
| `Pipeline::Start()` 中 SetCallback 逻辑 | Push 回调注册 | 不再需要 |

---

## 四、RecordingSink 重构

### 4.1 问题

当前 `RecordingSink` 在 `Probe()` 中硬编码创建：

```cpp
// feature_driver.h — Probe()
Status Probe() {
    // ...
    auto rec_sink = std::make_unique<RecordingSink>();
    // ... 配置 ...
    recording_sink_ = rec_sink.get();
    RecordingSinkRegistry::Instance().Register(Name(), recording_sink_);
    pipeline_->AddSink(std::move(rec_sink));
    // ...
}
```

**问题**：
1. 基类 `Probe()` 不应该知道 `RecordingSink` 的存在（违反单一职责原则）
2. 所有 Feature 都被强制安装 RecordingSink，不能选择
3. `RecordingSink` 的 `Write()` 在 `recording_=false` 时快速返回，但架构上不够干净

### 4.2 设计：RecordingSink 提升为 FeatureDriver 公开接口

#### 核心：动态装载/卸载 Sink Chain

Pipeline 当前设计：`SubmitToSinks()` 在 **ProcessThread（独占线程）** 中遍历 `sinks_` 向量。录制 API 在 **HTTP 线程** 中调用。需要轻量级并发保护。

**方案**：`std::shared_mutex`
- `SubmitToSinks()` 拿 `shared_lock`（读锁，多个读者可并发，开销为原子递增）
- `AddSinkRuntime()` / `RemoveSinkRuntime()` 拿 `unique_lock`（写锁，独占）

shared_lock 在热路径上开销极低，不会影响数据处理性能。

#### Pipeline 新增方法

```cpp
// pipeline.h
class Pipeline {
public:
    // ... 现有接口 ...

    // 运行时动态添加 Sink（线程安全，可并发调用）
    Status AddSinkRuntime(std::unique_ptr<SinkPlugin> sink) {
        std::unique_lock lock(sink_mutex_);
        sinks_.push_back(std::move(sink));
        return Status::Ok();
    }

    // 运行时动态移除 Sink（线程安全）
    Status RemoveSinkRuntime(const std::string& name) {
        std::unique_lock lock(sink_mutex_);
        auto it = std::remove_if(sinks_.begin(), sinks_.end(),
            [&name](const auto& s) { return s->Name() == name; });
        if (it != sinks_.end()) {
            sinks_.erase(it, sinks_.end());
            return Status::Ok();
        }
        return Status::Error(StatusCode::kNotFound, "sink not found: " + name);
    }

private:
    mutable std::shared_mutex sink_mutex_;  // 保护 sinks_ 动态修改

    // SubmitToSinks 改造：
    void SubmitToSinks(DataBatchPtr batch) {
        std::shared_lock lock(sink_mutex_);  // 轻量级读锁
        if (!sink_pool_) {
            for (auto& sink : sinks_) {
                auto status = sink->Write(batch);
                // ...
            }
            return;
        }
        // ... SinkPool 路径同样在 shared_lock 保护下 ...
    }
};
```

#### 目标架构

```
前端 → REST API → FeatureDriver::StartRecording() → 动态创建 RecordingSink
                                                      → Pipeline::AddSinkRuntime()
                  FeatureDriver::StopRecording()  → RecordingSink::StopRecording()
                                                      (保留 Sink 实例，下次复用)
                  FeatureDriver::IsRecording()    → 查询状态
```

#### FeatureDriver 新增接口

```cpp
// feature_driver.h
class FeatureDriver {
public:
    // ... 现有接口 ...

    // ---- 录制控制 ----
    // 开始录制：如 RecordingSink 不存在则动态创建并注入 Pipeline
    virtual Status StartRecording(const std::string& output_dir = "");
    // 停止录制：保留 RecordingSink 实例，下次可复用
    virtual Status StopRecording();
    // 查询录制状态
    virtual bool IsRecording() const;
    // 获取录制会话信息
    virtual RecordingSession GetRecordingSession() const;

protected:
    RecordingSink* recording_sink_ = nullptr;  // 保留，但从 Probe() 中移除初始化逻辑
};
```

#### 默认实现（动态装载/卸载 Sink Chain）

```cpp
// feature_driver.h — 实现（或在 .cc 文件中）
Status FeatureDriver::StartRecording(const std::string& output_dir) {
    if (!pipeline_) {
        return Status::Error(StatusCode::kUnavailable, "pipeline not running");
    }

    // 如果 RecordingSink 已存在，直接激活
    if (recording_sink_) {
        return recording_sink_->StartRecording();
    }

    // 动态创建 RecordingSink 并注入 Pipeline
    auto sink = std::make_unique<RecordingSink>();
    ConfigValue cfg;
    cfg.Set("feature_name", std::string(Name()));
    cfg.Set("output_dir", output_dir.empty()
        ? "/tmp/illuminator_data" : output_dir);
    sink->Init(cfg);

    auto status = sink->StartRecording();
    if (!status.ok()) return status;

    recording_sink_ = sink.get();
    RecordingSinkRegistry::Instance().Register(Name(), recording_sink_);

    // 动态注入 Pipeline（Pipeline::AddSinkRuntime 使用 unique_lock 保护）
    return pipeline_->AddSinkRuntime(std::move(sink));
}

Status FeatureDriver::StopRecording() {
    if (!recording_sink_) {
        return Status::Error(StatusCode::kInvalidArgument, "not recording");
    }
    auto status = recording_sink_->StopRecording();
    // 保留 RecordingSink 在 Pipeline 中，下次 StartRecording 可直接复用
    return status;
}

bool FeatureDriver::IsRecording() const {
    return recording_sink_ && recording_sink_->IsRecording();
}

RecordingSession FeatureDriver::GetRecordingSession() const {
    if (recording_sink_) return recording_sink_->GetSession();
    return {};
}
```

#### Probe() 清理

```cpp
// feature_driver.h — Probe() 改造后
Status Probe() {
    if (state_ != DriverState::kInactive) {
        return Status::Error(StatusCode::kInvalidArgument,
                             std::string(Name()) + " already active");
    }

    auto& infra = InfrastructureManager::Instance();
    if (!infra.IsStarted()) {
        return Status::Error(StatusCode::kUnavailable,
                             "InfrastructureManager not started");
    }

    pipeline_ = BuildPipeline(infra);
    if (!pipeline_) {
        return Status::Error(StatusCode::kInternal,
                             std::string(Name()) + " BuildPipeline returned null");
    }

    // 【移除】不再在这里创建 RecordingSink
    // RecordingSink 由 StartRecording() 按需动态创建

    pipeline_->SetSinkPool(infra.GetSinkPool());
    auto status = pipeline_->Start();
    if (!status.ok()) {
        pipeline_.reset();
        return status;
    }

    RegisterTimers(infra);

    state_ = DriverState::kActive;
    start_time_ = std::chrono::steady_clock::now();
    IL_INFO("FeatureDriver '{}' probed successfully", Name());
    return Status::Ok();
}
```

#### Remove() 清理

```cpp
Status Remove() {
    if (state_ == DriverState::kInactive) return Status::Ok();

    UnregisterTimers(InfrastructureManager::Instance());

    // 清理 RecordingSink 注册
    if (recording_sink_) {
        RecordingSinkRegistry::Instance().Unregister(Name());
        recording_sink_ = nullptr;  // Pipeline 拥有所有权，这里只是裸指针
    }

    if (pipeline_) {
        pipeline_->Stop();
        pipeline_.reset();
    }

    state_ = DriverState::kInactive;
    IL_INFO("FeatureDriver '{}' removed", Name());
    return Status::Ok();
}
```

#### API 路由简化

```cpp
// api_routes.h — 录制 API 简化后
// 不再需要 RecordingSinkRegistry，直接通过 FeatureBus 访问 FeatureDriver

srv.Post("/api/v1/features/:name/record/start",
    [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.path_params.at("name");
        auto* drv = FeatureBus::Instance().GetDriver(name);
        if (!drv) {
            JsonError(res, "feature not found: " + name, 404);
            return;
        }

        // 可选：从请求体读取 output_dir
        std::string output_dir;
        if (!req.body.empty()) {
            auto body = nlohmann::json::parse(req.body);
            output_dir = body.value("output_dir", "");
        }

        auto status = drv->StartRecording(output_dir);
        if (!status.ok()) {
            JsonError(res, status.message(), 400);
            return;
        }
        auto session = drv->GetRecordingSession();
        res.set_content(
            json{{"status", "ok"}, {"feature", name},
                 {"file", session.file_path}}.dump() + "\n",
            "application/json");
    });

srv.Post("/api/v1/features/:name/record/stop",
    [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.path_params.at("name");
        auto* drv = FeatureBus::Instance().GetDriver(name);
        if (!drv) {
            JsonError(res, "feature not found: " + name, 404);
            return;
        }
        auto status = drv->StopRecording();
        if (!status.ok()) {
            JsonError(res, status.message(), 400);
            return;
        }
        auto session = drv->GetRecordingSession();
        res.set_content(
            json{{"status", "ok"}, {"feature", name},
                 {"file", session.file_path},
                 {"batches", session.batches_written},
                 {"bytes", session.bytes_written}}.dump() + "\n",
            "application/json");
    });

srv.Get("/api/v1/features/:name/record/status",
    [](const httplib::Request& req, httplib::Response& res) {
        auto name = req.path_params.at("name");
        auto* drv = FeatureBus::Instance().GetDriver(name);
        if (!drv) {
            res.set_content(
                json{{"feature", name}, {"recording", false}}.dump() + "\n",
                "application/json");
            return;
        }
        bool recording = drv->IsRecording();
        auto session = drv->GetRecordingSession();
        json j;
        j["feature"] = name;
        j["recording"] = recording;
        if (recording) {
            j["file"] = session.file_path;
            j["bytes_written"] = session.bytes_written;
            j["batches_written"] = session.batches_written;
        }
        res.set_content(j.dump() + "\n", "application/json");
    });
```

#### 全局录制 API 保持不变

```cpp
// 全局录制仍然通过 RecordingSinkRegistry 遍历所有 Feature
srv.Post("/api/v1/recording/start",
    [](const httplib::Request&, httplib::Response& res) {
        auto& registry = RecordingSinkRegistry::Instance();
        // ... 遍历所有已注册的 RecordingSink ...
    });
```

### 4.3 SseSink 保持不变

SseSink 已经在各 Driver 的 `BuildPipeline()` 中正常加载，架构合理，无需改动：

```cpp
// cpu_profiler_driver.h — BuildPipeline()
pipeline->AddSink(std::make_unique<SseSink>("cpu_profiler"));
```

---

## 五、其他建议接口（暂缓）

以下接口建议在后续迭代中考虑，本次不实施：

- **Health / Diagnostics**：`Diagnose()` 返回 Pipeline 各阶段健康状态
- **Shutdown vs Remove**：`Shutdown()` 优雅关闭（先排空再停止），`Reset()` 热重启

---

## 六、接口总览

```
FeatureDriver 接口全景（v2）：

┌─ 标识与元数据 ───────────────────────────────────┐
│ Name() / DisplayName() / Version() / Category() / Tier() │
│ Describe() / ConfigSchema() / GetConfig() / SetConfig()  │
├─ 生命周期 ───────────────────────────────────────┤
│ Probe()          — 构建 Pipeline 并启动          │
│ Remove()         — 停止并销毁 Pipeline           │
│ Pause()          — 暂停采集（取消 TimerWheel -> 一行搞定）│
│ Resume()         — 恢复采集（恢复 TimerWheel -> 一行搞定）│
├─ 运行时重配置 ───────────────────────────────────┤
│ Reconfigure() [增强] — Driver 自主分发到各 Pipeline 阶段 │
├─ 录制控制 ───────────────────────────────────────┤
│ StartRecording() [新增] — 动态创建/激活 RecordingSink │
│ StopRecording()  [新增] — 停用录制               │
│ IsRecording()    [新增] — 查询录制状态           │
│ GetRecordingSession() [新增] — 获取会话信息       │
├─ 诊断与统计 ─────────────────────────────────────┤
│ GetStats()         — 运行统计                    │
│ Info()             — 运行时信息                  │
│ State()            — 状态查询                    │
│ GetPipeline()      — Pipeline 访问               │
└────────────────────────────────────────────────┘
```

---

## 七、实施计划

### Phase 1：基础接口增强（无破坏性变更，独立可测）

1. **Pipeline 新增访问器**：`GetProcessors()`、`GetAggregator()`、`GetSinks()`
2. **Pipeline 新增 Sink 动态管理**：`AddSinkRuntime()`、`RemoveSinkRuntime()` + `shared_mutex` 保护 `SubmitToSinks()`
3. **ProcessorPlugin / AggregatorPlugin / SinkPlugin 新增 `Reconfigure()` 虚方法**（默认空实现/返回 Ok）

### Phase 2：eBPF 插件去掉后台线程 + 统一 TimerWheel 驱动

1. **CpuProfilerSource aggregated 模式**：
   - 去掉 `agg_thread_` 和 `AggregatedPullLoop()`
   - `Collect()` 改为同步读 `stack_counts` HASH map + `stacks` STACK_TRACE map
   - `IntervalMs()` 返回 `aggregate_interval_ms_`
2. **OffcpuProfilerSource**：
   - 去掉 `poll_thread_`、`ring_buf_`、`HandleEvent` 回调
   - 去掉 BPF 侧 ring buffer 代码
   - `Collect()` 改为同步读 `offcpu_stats` HASH map + `offcpu_stacks` STACK_TRACE map
   - `IntervalMs()` 返回 `1000`
3. **CpuProfilerSource stream 模式**：
   - 去掉 `poll_thread_`、`callback_`、`SetCallback()`
   - `Collect()` 改为非阻塞 `ring_buffer__poll(0)` 排空 + 累积到 batch
   - `IntervalMs()` 返回 `100`（每 100ms 排空一次）
4. **SourcePlugin 接口精简**：移除 `SetCallback()`、`IsPushMode()`、`PauseDataFlow()`、`ResumeDataFlow()`、`flow_paused_`
5. **Pipeline::Start() 精简**：移除 `SetCallback()` 逻辑

### Phase 3：FeatureDriver 接口改造

1. **Reconfigure 改为 Driver 自主分发**：基类默认实现遍历所有 Pipeline 阶段分发参数
2. **Pause/Resume 简化**：仅 `UnregisterTimers()` / `RegisterTimers()`（所有插件统一 Pull）
3. **新增录制控制接口**：`StartRecording()` / `StopRecording()` / `IsRecording()` / `GetRecordingSession()`

### Phase 4：RecordingSink 从 Probe 中移除

1. **Probe() 移除 RecordingSink 创建逻辑** — 不再在基类中硬编码
2. **Remove() 保留 RecordingSink 注册清理逻辑** — 仅清理裸指针引用
3. **API 路由改为通过 FeatureBus 访问 FeatureDriver 的录制接口** — 不再直接依赖 RecordingSinkRegistry

### Phase 5：清理和验证

1. 编译通过
2. 全部 29 个单元测试通过
3. 手动验证：
   - Pause/Resume 对所有插件正常工作（统一 `UnregisterTimers()`）
   - CpuProfiler stream 模式非阻塞 poll 工作正常
   - 录制 API 通过 FeatureDriver 正常创建/激活 RecordingSink
   - Sink chain 动态添加/移除不影响正常数据处理
   - eBPF 插件重构后数据采集正确性不变