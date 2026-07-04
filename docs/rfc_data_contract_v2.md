# RFC: Illuminator 架构重设计 v3 — Feature 驱动模型 + 极简数据流

> **状态**: 已实现 (Implemented)  
> **作者**: Illuminator Architecture Team  
> **日期**: 2026-07-02  
> **版本**: v3 (整合通道分离、SSE 替代 WebSocket、Buffer 移除、前端功能设计)  
> **目标**: 建立 Linux 驱动模型风格的三层架构，根治 FeatureManager 上帝类问题，实现极简数据流
>
> **实现说明** (2026-07-02): 核心三层架构已完全实现并通过全部测试。实现过程中对部分设计做了优化调整，  
> 主要差异记录在文档末尾的 "§11 实现偏差记录" 中。

---

## 摘要 (Executive Summary)

### 我们要解决什么问题？

当前 Illuminator 架构存在以下核心问题：

1. **FeatureManager 是上帝类 (1200 行)**：`FeatureManager::Start()` 承担了 8 项职责——创建 Plugin、组装 Pipeline、合并参数、注册定时器、注入 Sink、启动 Pipeline、状态管理、安全保护。每增加一种 Feature 类型，都需要修改 FeatureManager 内部的逻辑。

2. **Pipeline 和 Feature 是人为分离的**：`FeatureManager` 持有 `PipelineController` 引用，Pipeline 由 `PipelineController` 从 YAML 构建，然后 FeatureManager 又从 YAML 中读取配置重新注册 Feature。Feature 和 Pipeline 本质上是同一个东西，但被分到了两个管理器中。

3. **PipelineController 职责过重**：它管理 TimerWheel、CollectPool、SinkPool、所有 Pipeline 的构建和启动、定时器注册、指标同步。但实际上，它只是一个"基础设施管理器"加上"Pipeline 集合管理器"。

4. **前端无法自动发现 Feature 能力**：当前 Feature 的元数据（category、tier、display_name）通过 `main.cc` 中的硬编码 lambda 函数映射。新增 Feature 需要修改 `main.cc`。前端无法自动知道某个 Feature 需要什么参数（如 `target_pids`）。

5. **WebSocket 轮询冒充推送**：`WebSocketManager::BroadcastLoop` 以 1 秒间隔轮询 `StreamSinkStore`，与 HTTP 轮询没有本质区别。延迟 0-1000ms。

6. **上帝类 API 路由文件**：`api_routes.h` (1200 行) 包含所有 API 端点注册，职责过重。

7. **StreamSinkStore (Buffer) 多余**：实时推送不需要 buffer，导出/录制从此刻开始即可，不需要回溯历史数据。前端断线重连后从头开始接收数据即可（Grafana 行为）。

8. **SinkPool 过载时直接丢弃数据**：当 `PendingTasks() > 256` 时丢弃数据。当前方案保持单 SinkPool + 队列满丢弃，后续可考虑分离池优化。

### 核心设计决策

```
┌──────────────────────────────────────────────────────────────────┐
│                    三层架构 (Linux 驱动模型风格)                      │
│                                                                    │
│  Layer 1: InfrastructureManager (基础设施层)                        │
│    只管理 TimerWheel + CollectPool + SinkPool                        │
│    不知道什么是 Feature，什么是 Pipeline                              │
│    80 行                                                            │
│                                                                    │
│  Layer 2: FeatureBus (框架总线层)                                    │
│    只管理 Feature 的注册和生命周期编排                                │
│    不知道什么是 Plugin，什么是 Pipeline                               │
│    200 行                                                           │
│                                                                    │
│  Layer 3: FeatureDriver (Feature 实例层)                             │
│    每个 Feature 实现标准接口，自己管理自己的 Pipeline                  │
│    知道自己的 Plugin、Pipeline、定时器                                │
│    每个 Feature 50-70 行，共 10 个 ≈ 600 行                          │
│                                                                    │
│  总计: 80 + 200 + 600 = 880 行                                      │
│  当前: FeatureManager 1200 + PipelineController 600 = 1800          │
│  减少: 51%                                                          │
│                                                                    │
│  数据流: 极简 (SSE 直推，无 Buffer，无 WebSocket)                     │
│  当前: StreamSinkStore 340 + WebSocketManager 600 = 940             │
│  新方案: SseHandler 150 行                                          │
│  减少: 71%                                                          │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘
```

### 与 Linux 驱动模型的对照

| Linux 概念 | Illuminator 对应 | 说明 |
|-----------|-----------------|------|
| `struct device_driver` | `FeatureDriver` 基类 | 驱动抽象，定义标准接口 |
| `struct device` | Feature 实例 | 每个 Feature 是一个设备实例 |
| `driver_register()` | `REGISTER_FEATURE()` | 注册 Feature 类型 |
| `device_register()` | `FeatureBus::RegisterFeature()` | 注册 Feature 实例 |
| `bus_type.match()` | Feature 类型名匹配 | 通过 type 名找到对应的 Feature 工厂 |
| `probe()` | `FeatureDriver::OnStart()` | 初始化设备，创建 Pipeline |
| `remove()` | `FeatureDriver::OnStop()` | 释放设备资源 |
| `file_operations.read` | `FeatureDriver::GetStats()` | 获取数据 |
| `file_operations.ioctl` | `FeatureDriver::SetConfig()` | 运行时配置 |
| `sysfs` | REST API | 对外暴露标准接口 |
| `struct device_attribute` | `FeatureDescriptor` | 设备属性自描述 |
| `uevent` | `StateChangeCallback` | 状态变更通知 |
| `suspend/resume` | `OnPause()/OnResume()` | 暂停/恢复 |
| `class` | `FeatureTier` | 设备分类 |

---

## 一、当前架构问题详解

### 1.1 main.cc 中的硬编码映射

```cpp
// 当前 main.cc — 硬编码的 Feature 元数据
// 每新增一个 Feature，需要修改 main.cc 的三个 lambda 函数
auto resolve_category = [](const std::string& name) -> std::string {
    if (name.find("cpu") != std::string::npos || ...) return "cpu";
    // ...
};
auto resolve_display_name = [](const std::string& name) -> std::string {
    if (name == "cpu_utilization") return "CPU Utilization";
    // ...
};
auto resolve_tier = [](const std::string& name) -> illuminator::FeatureTier {
    if (name == "cpu_utilization" || ...) return illuminator::FeatureTier::kMonitoring;
    // ...
};
```

**问题**：新增 Feature 需要修改 `main.cc`，而这应该是 Feature 自己的元数据。

### 1.2 FeatureManager::Start() 的 8 项职责

```cpp
// FeatureManager::Start() 当前实现
Status Start(const std::string& name, const StartParams& params = {}) {
    // 1. 参数校验
    // 2. 在线重配置（如果已 Active）
    // 3. 状态合法性检查
    // 4. 如果 Paused → Resume
    // 5. Tier 3 安全检查
    // 6. 状态切换 → Starting
    // 7. CreateAndStartPipeline() ← 这又做了 10 件事
    //    7a. 创建 Source 插件
    //    7b. 合并 StartParams 到 Source 配置
    //    7c. 创建 Processor 插件
    //    7d. 创建 Sink 插件
    //    7e. 注入 StreamSink
    //    7f. 注入 RecordingSink
    //    7g. 设置 SinkPool
    //    7h. 启动 Pipeline
    //    7i. 注册 Collect 定时器
    //    7j. 注册 Flush 定时器
    // 8. 状态切换 → Active
}
```

### 1.3 WebSocket 广播是轮询

```cpp
// WebSocketManager::BroadcastLoop() — 1 秒间隔轮询
// 从 StreamSinkStore 拉取数据 → 去重 → 序列化 → 写入 fd
// 这不是推送，是轮询。延迟 0-1000ms。
```

### 1.4 StreamSinkStore (Buffer) 多余

```
当前数据流（复杂，3 层中间层）：
  Pipeline → SinkPool → StreamSink → StreamSinkStore (buffer)
    → WebSocketManager::BroadcastLoop (轮询) → 序列化 → write(fd)

新数据流（极简，直达）：
  Pipeline → SinkPool → StreamSink → SseHandler::Push (条件变量) → write(fd)
```

**为什么 Buffer 不需要？**

| 场景 | 需要 Buffer 吗？ | 原因 |
|------|-----------------|------|
| 实时推送 | 不需要 | 数据到达直接 SSE 推送 |
| 前端重连 | 不需要 | 重连后从头开始接收（Grafana 行为） |
| 页面刷新 | 不需要 | 同上 |
| 导出 | 不需要 | 纯前端操作（ringBuffer 序列化下载） |
| 录制 | 不需要 | 从此刻开始录制，不需要回溯 |
| 多标签页 | 不需要 | 新标签页从头开始，各自独立 |

**唯一需要 Buffer 的场景**：导出超长时间范围数据（如最近 30 分钟）。这是高级功能，不是核心需求。

### 1.5 SinkPool 过载时直接丢弃所有数据

```cpp
// 当前：kMaxPendingTasks = 256 时丢弃所有数据（包括实时推送）
if (sink_pool_->PendingTasks() > kMaxPendingTasks) {
    // 丢弃！SSE 推送数据也丢了
    return;
}
```

**问题**：慢 Sink（如 NFS 文件 I/O）阻塞线程池时，实时推送数据也被丢弃。

---

## 二、三层架构设计

### 2.1 Layer 1: InfrastructureManager（基础设施层）

```cpp
// ============================================================================
// InfrastructureManager — 基础设施管理器
// ============================================================================
//
// 这是最底层的组件，只管理三样东西：定时器、采集线程池、写入线程池。
// 它不知道什么是 Feature、什么是 Pipeline、什么是 Plugin。
//
// 职责：
//   1. 管理 TimerWheel（全局定时调度器）
//   2. 管理 CollectPool（采集线程池）
//   3. 管理 SinkPool（写入线程池，队列满时丢弃）
//   4. 管理基础设施的启动和停止
//
// 不负责：
//   ✗ 构建 Pipeline — 这是 Feature 自己的事
//   ✗ 注册定时器 — 这是 Feature 自己的事
//   ✗ 管理 Feature 集合 — 这是 FeatureBus 的事
// ============================================================================

class InfrastructureManager {
public:
    static InfrastructureManager& Instance();

    Status Start();
    Status Stop();

    TimerWheel& GetTimerWheel()  { return timer_; }
    ThreadPool* GetCollectPool() { return collect_pool_.get(); }
    ThreadPool* GetSinkPool()    { return sink_pool_.get(); }

private:
    TimerWheel timer_;
    std::unique_ptr<ThreadPool> collect_pool_;  // 采集线程池
    std::unique_ptr<ThreadPool> sink_pool_;     // 写入线程池（队列满时丢弃）
};
```

**代码量**: ~80 行

**关键设计选择**：保持单 SinkPool + 队列满丢弃，与当前行为一致。

**后续优化方向**（不在本次 RFC 范围内）：
- 分离池：将关键通道（SSE 推送）和非关键通道（文件 I/O）分配到不同线程池
- 阻塞队列：当线程池满时阻塞生产者而非丢弃

**关于 SSE 推送的线程模型**：

SSE 推送不是在 SinkPool 中完成的。SinkPool 线程只做快速内存操作（push 到队列 + 条件变量通知），不写 fd。写 fd 的是 httplib 为每个 SSE 连接创建的 HTTP 请求处理线程。

```
SinkPool 线程（快，微秒级）：
  StreamSink::Write() → pending_.push(batch) + cv_.notify_one()
  只做内存操作，不写 fd，不阻塞

HTTP 请求线程（可能慢，毫秒级）：
  SseLoop() 等待条件变量 → 序列化 → sink.write(fd)
  真正写 fd，慢客户端在此阻塞
  但阻塞的是 HTTP 线程，不影响 SinkPool
```

即使客户端 TCP 缓冲区满了，`sink.write()` 阻塞，阻塞的也是 HTTP 请求处理线程，SinkPool 线程完全不受影响。因此不需要分离 SinkPool 来保护 SSE 推送。

### 2.2 Layer 2: FeatureBus（框架总线层）

```cpp
// ============================================================================
// FeatureBus — Feature 框架总线
// ============================================================================
//
// 实际实现 (src/core/engine/feature_bus.h)
//
// 职责：
//   1. 注册 FeatureDriver 实例
//   2. 编排 Driver 生命周期（Probe/Remove/Pause/Resume）
//   3. 提供统一的 Driver 查询和元数据接口
//   4. 自动 Probe Tier 1-2 Driver
//   5. 通知状态变更
//
// 不负责：
//   ✗ 创建 Plugin — FeatureDriver::BuildPipeline() 自己负责
//   ✗ 组装 Pipeline — FeatureDriver::BuildPipeline() 自己负责
//   ✗ 注册定时器 — FeatureDriver::Probe() 自己负责
//   ✗ 管理 SSE 连接 — SseHandler 独立管理
//   ✗ 录制控制 — RecordingSinkRegistry + REST 独立管理
// ============================================================================

class FeatureBus {
public:
    static FeatureBus& Instance();

    // ---- Driver 注册 ----
    Status Register(std::unique_ptr<FeatureDriver> driver);
    Status Unregister(const std::string& name);

    // ---- Driver 生命周期 (Linux probe/remove 语义) ----
    Status Probe(const std::string& name);
    Status Remove(const std::string& name);
    Status Pause(const std::string& name);
    Status Resume(const std::string& name);

    // ---- Driver 查询 ----
    std::vector<DriverInfo> ListDrivers() const;
    std::vector<FeatureDescriptor> ListDescriptors() const;
    DriverState GetState(const std::string& name) const;
    FeatureDriver* GetDriver(const std::string& name);

    // ---- Driver 配置（前端自动发现） ----
    std::string GetConfigSchema(const std::string& name) const;  // JSON Schema
    std::string GetConfig(const std::string& name) const;        // 当前配置值
    Status SetConfig(const std::string& name, const std::string& json_config);

    // ---- Auto-Probe (Tier 1-2) ----
    Status ProbeAll();

    // ---- 状态通知 ----
    void SetStateChangeCallback(StateChangeCallback cb);

    void RemoveAll();

private:
    std::unordered_map<std::string, std::unique_ptr<FeatureDriver>> drivers_;
    StateChangeCallback state_callback_;
    mutable std::shared_mutex mutex_;
};
```

**代码量**: ~259 行

### 2.3 Layer 3: FeatureDriver 基类

```cpp
// ============================================================================
// FeatureDriver — 所有 Feature 的统一基类
// ============================================================================
//
// FeatureDriver 是 Illuminator 驱动模型的核心抽象。每个 Feature 都是一个完整的、
// 自包含的数据处理单元，包含 Pipeline 和标准控制接口。
//
// 每个 FeatureDriver 子类只需要实现：
//   1. 元数据方法 — Name(), DisplayName(), Category(), Tier(), Describe()
//   2. Pipeline 构建 — BuildPipeline(InfrastructureManager&) [纯虚]
//   3. 配置接口 — ConfigSchema(), Reconfigure()
//
// 基类自动处理：
//   4. Probe() — 调用 BuildPipeline + 注册定时器 + 附加 RecordingSink + 附加 SseSink
//   5. Remove() — 停止 Pipeline + 注销定时器 + 注销 RecordingSink
//   6. Pause()/Resume() — 暂停/恢复定时器
// ============================================================================

class FeatureDriver {
public:
    virtual ~FeatureDriver() = default;

    // ========================================================================
    // 1. 标识与元数据 (子类必须覆写)
    // ========================================================================

    virtual const char* Name() const = 0;        // "cpu_utilization"
    virtual const char* DisplayName() const = 0; // "CPU Utilization"
    virtual const char* Category() const = 0;    // "cpu" | "memory" | "io" | "network" | "gpu"
    virtual DriverTier Tier() const = 0;         // kMonitoring | kTracing | kProfiling

    // Describe — 返回 Feature 的自描述信息（可覆写扩展）
    virtual FeatureDescriptor Describe() const;

    // ========================================================================
    // 2. Pipeline 构建 (子类必须覆写)
    // ========================================================================

    virtual std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) = 0;

    // ========================================================================
    // 3. 配置接口（前端自动生成配置表单）
    // ========================================================================

    virtual std::string ConfigSchema() const {
        return R"({"type":"object","properties":{}})";
    }
    virtual Status Reconfigure(const ConfigValue& params) {
        return Status::Error(StatusCode::kUnimplemented, "not supported");
    }

    // ========================================================================
    // 4. 生命周期 (基类实现, 子类一般不覆写)
    // ========================================================================

    // Probe: 调用 BuildPipeline → 附加 SseSink/RecordingSink → 注册定时器 → 启动
    Status Probe();

    // Remove: 停止 Pipeline → 注销定时器 → 注销 RecordingSink
    Status Remove();

    Status Pause();
    Status Resume();

    DriverState State() const;

protected:
    std::unique_ptr<Pipeline> pipeline_;
    RecordingSink* recording_sink_ = nullptr;
};
```

**代码量**: ~250 行 (含 Probe/Remove 实现)

### 2.4 FeatureDescriptor — 自描述结构

```cpp
struct FeatureDescriptor {
    std::string name;              // "cpu_utilization"
    std::string display_name;      // "CPU 利用率"
    std::string description;       // 功能描述（tooltip）
    std::string category;          // "cpu" | "memory" | "io" | "network" | "gpu"
    std::string version;
    FeatureTier tier = FeatureTier::kMonitoring;
    DataModelType model = DataModelType::TIME_SERIES;

    // 能力声明
    bool supports_pull = true;
    bool supports_push = false;
    bool supports_pause = false;
    bool supports_configure = false;
    bool has_bpf_probe = false;
    bool session_required = false;   // Tier 3: 需要 Session

    // 参数声明（前端自动生成配置表单）
    std::vector<ParamDeclaration> params;
};

struct ParamDeclaration {
    std::string key;
    std::string display_name;
    ParamType type;              // kString | kInteger | kIntegerRange | kPidList | kEnum | kBoolean
    bool required = false;
    std::string default_value;
    std::string description;
    int min_value = 0;
    int max_value = 0;
    std::vector<std::string> choices;
};

struct FeatureStats {
    uint64_t batches_processed = 0;
    uint64_t records_processed = 0;
    uint64_t errors = 0;
    uint64_t uptime_ms = 0;
};
```

### 2.5 FeatureDriver 实现示例

```cpp
// ============================================================================
// CpuUtilizationDriver — 最简单的 Feature 实现
// ============================================================================

class CpuUtilizationDriver : public FeatureDriver {
public:
    const char* Name() const override { return "cpu_utilization"; }
    const char* Version() const override { return "2.0.0"; }
    const char* Category() const override { return "cpu"; }
    FeatureTier Tier() const override { return FeatureTier::kMonitoring; }

    FeatureDescriptor Describe() const override {
        FeatureDescriptor desc;
        desc.name = "cpu_utilization";
        desc.display_name = "CPU 利用率";
        desc.description = "系统级 CPU 利用率指标（user/system/idle/iowait）";
        desc.category = "cpu";
        desc.tier = FeatureTier::kMonitoring;
        desc.model = DataModelType::TIME_SERIES;
        desc.supports_pause = true;
        return desc;
    }

    std::string GetConfigSchema() const override {
        return R"({
            "type": "object",
            "properties": {
                "interval_ms": {
                    "type": "integer", "default": 1000, "minimum": 100, "maximum": 10000,
                    "description": "采集间隔(毫秒)"
                },
                "collect_per_core": {
                    "type": "boolean", "default": true,
                    "description": "是否按核心拆分"
                }
            }
        })";
    }

    std::string GetConfig() const override {
        return fmt::format(R"({{"interval_ms":{},"collect_per_core":{}}})",
                           interval_ms_, collect_per_core_);
    }

    Status SetConfig(const std::string& json_config) override {
        // 解析 JSON 并更新配置
        return Status::Ok();
    }

    Status OnStart(const FeatureParams& params) override {
        pipeline_ = std::make_unique<Pipeline>(Name());
        auto source = std::make_unique<CpuUtilizationSource>();
        source->Init(/* config */);
        pipeline_->SetSource(std::move(source));

        // 注册定时器
        auto& timer = bus_->GetInfra().GetTimerWheel();
        timer_ids_.push_back(timer.AddRepeating(
            std::chrono::milliseconds(interval_ms_),
            [this]() { /* 采集逻辑 */ }
        ));

        return Status::Ok();
    }

    Status OnStop() override {
        // 取消定时器
        auto& timer = bus_->GetInfra().GetTimerWheel();
        for (auto tid : timer_ids_) timer.Cancel(tid);
        timer_ids_.clear();
        return Status::Ok();
    }

private:
    uint32_t interval_ms_ = 1000;
    bool collect_per_core_ = true;
    std::vector<uint32_t> timer_ids_;
};

// 自动注册宏
REGISTER_FEATURE(CpuUtilizationDriver);
```

---

## 三、多消费者 Sink 设计：保持当前 SinkFanout

### 3.1 核心概念

当前 `SinkFanout` 已经支持多消费者（shared_ptr 零拷贝分发），每个 Pipeline 可配置多个 Sink。Pipeline 的 `SubmitToSinks` 将数据提交到 SinkPool 中并行执行。

**本次 RFC 不改变 SinkFanout 的设计**，保持单 SinkPool + 队列满丢弃（`kMaxPendingTasks = 256`）。

### 3.2 后续优化方向（不在本次 RFC 范围内）

- **SinkRouter**：引入通道优先级和策略模式（`kAlways` / `kOnDemand` / `kTriggered`），前端可动态控制通道开关
- **分离池**：将关键通道（SSE 推送）和非关键通道（文件 I/O）分配到不同线程池，避免慢 I/O 影响实时推送

### 3.3 与当前 SinkFanout 的对比

| 维度 | 当前 SinkFanout | 后续 SinkRouter |
|------|----------------|----------------|
| 分发方式 | 串行遍历所有子 Sink | 按优先级分线程池 |
| 线程池 | 共享一个 SinkPool | 关键/非关键分离 |
| 过载保护 | 队列满丢弃（256） | 关键通道不丢，非关键可丢 |
| 前端控制 | 不支持 | 支持动态开关通道 |
| 策略模式 | 无 | kAlways / kOnDemand / kTriggered |

---

## 四、数据流设计：SSE 替代 WebSocket + 移除 Buffer

---

## 四、数据流设计：SSE 替代 WebSocket + 移除 Buffer

### 4.1 极简数据流

```
当前（复杂，3 层中间层，轮询）：
  Pipeline → SinkPool → StreamSink → StreamSinkStore (340 行 Buffer)
    → WebSocketManager::BroadcastLoop (600 行，1 秒轮询)
    → 序列化 → write(fd)

新方案（极简，直达，事件驱动）：
  Pipeline → SinkPool → StreamSink → SseHandler::Push
    → 序列化 → write(fd)  (条件变量通知，延迟 < 100ms)
```

**去掉的东西**：
- StreamSinkStore (340 行) — 不需要 buffer
- WebSocketManager (600 行) — 用 SSE 替代
- WebSocketServer (260 行) — 同上
- 前端 Link Chain (360 行) — 用 DataBus 替代

**新增的东西**：
- SseHandler (150 行) — 极简 SSE 推送
- 前端 DataBus (60 行) — 极简 SSE 消费

**代码量对比**：~1560 行 → ~210 行，减少 87%

### 4.2 SseHandler 实现

```cpp
// ============================================================================
// SseHandler — 极简 SSE 推送
// ============================================================================
//
// 用 httplib 的 SSE 支持，条件变量通知替代轮询。
//
// 数据流：
//   StreamSink::Write() → SseHandler::Notify(DataBatchPtr)
//     → condition_variable 通知 → SseHandler::Push() → write(fd)
// ============================================================================

class SseHandler {
public:
    // 注册 SSE 路由到 httplib::Server
    void RegisterRoutes(httplib::Server& svr) {
        svr.Get("/api/v1/events", [this](const httplib::Request& req,
                                          httplib::Response& res) {
            // 设置 SSE 头
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");  // 禁用 nginx 缓冲

            // 设置 chunked 响应
            res.set_chunked_content_provider(
                "text/event-stream",
                [this](size_t offset, httplib::DataSink& sink) -> bool {
                    return SseLoop(sink);
                }
            );
        });
    }

    // 数据到达通知（由 StreamSink 调用）
    void Notify(DataBatchPtr batch) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(batch);
        }
        cv_.notify_one();
    }

    // 注册连接
    void RegisterConnection(httplib::DataSink* sink) {
        current_sink_ = sink;
    }

    void UnregisterConnection() {
        current_sink_ = nullptr;
    }

private:
    bool SseLoop(httplib::DataSink& sink) {
        RegisterConnection(&sink);

        while (true) {
            std::vector<DataBatchPtr> batches;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !pending_.empty() || stop_; });
                if (stop_) return false;
                batches.swap(pending_);
            }

            for (auto& batch : batches) {
                std::string json = SerializeToSse(batch);
                sink.write(json.data(), json.size());
            }
        }
    }

    std::string SerializeToSse(DataBatchPtr batch) {
        std::string json = DataSerializer::ToJson(*batch);
        return "data: " + json + "\n\n";
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<DataBatchPtr> pending_;
    httplib::DataSink* current_sink_ = nullptr;
    bool stop_ = false;
};
```

**代码量**: ~150 行

### 4.3 前端 DataBus

```typescript
// ============================================================================
// DataBus — 极简 SSE 数据消费层
// ============================================================================
//
// 替代当前复杂的 LiveDataSource Link Chain（360 行），用 60 行实现。
//
// 功能：
//   - 建立 SSE 连接
//   - 解析 JSON 数据
//   - 维护 ringBuffer（用于前端导出和时间窗口选择）
//   - 按 modelType 分发到对应的 React hooks
// ============================================================================

type DataListener = (batch: DataBatch) => void

class DataBus {
  private eventSource: EventSource | null = null
  private listeners = new Map<string, Set<DataListener>>()
  private ringBuffer: DataBatch[] = []  // 前端 ringBuffer
  private maxBufferSize = 60

  constructor(private url: string = '/api/v1/events') {}

  connect() {
    this.eventSource = new EventSource(this.url)
    this.eventSource.onmessage = (event) => {
      const batch: DataBatch = JSON.parse(event.data)
      this.ringBuffer.push(batch)
      if (this.ringBuffer.length > this.maxBufferSize) {
        this.ringBuffer.shift()
      }
      this.notifyListeners(batch)
    }
    this.eventSource.onerror = () => {
      // 自动重连（SSE 内置）
    }
  }

  subscribe(feature: string, listener: DataListener) {
    if (!this.listeners.has(feature)) {
      this.listeners.set(feature, new Set())
    }
    this.listeners.get(feature)!.add(listener)
    return () => this.listeners.get(feature)?.delete(listener)
  }

  private notifyListeners(batch: DataBatch) {
    this.listeners.get(batch.feature)?.forEach(fn => fn(batch))
  }

  // 获取最近 N 条数据（前端导出用）
  getRecent(feature: string, count: number): DataBatch[] {
    return this.ringBuffer
      .filter(b => b.feature === feature)
      .slice(-count)
  }

  // 设置时间窗口（30s / 1min / 2min）
  setWindowSize(seconds: number) {
    // 根据采集间隔估算需要的 buffer 大小
    this.maxBufferSize = Math.ceil(seconds * 2)  // 假设 500ms 采集间隔
  }

  disconnect() {
    this.eventSource?.close()
  }
}

export const dataBus = new DataBus()
```

**代码量**: ~60 行

### 4.4 前端类型安全 Hook

```typescript
// 泛型 Hook，根据 modelType 自动推断数据类型
function useMetricsData(feature: string): MetricsBatch[] {
  const [data, setData] = useState<MetricsBatch[]>([])
  useEffect(() => {
    return dataBus.subscribe(feature, (batch) => {
      if (batch.modelType === 'time_series') {
        setData(prev => [...prev.slice(-60), batch as MetricsBatch])
      }
    })
  }, [feature])
  return data
}

function useProfileData(feature: string): ProfileBatch[] {
  const [data, setData] = useState<ProfileBatch[]>([])
  useEffect(() => {
    return dataBus.subscribe(feature, (batch) => {
      if (batch.modelType === 'profile') {
        setData(prev => [...prev.slice(-10), batch as ProfileBatch])
      }
    })
  }, [feature])
  return data
}
```

---

## 五、前端功能设计

### 5.1 功能 1：保存前端显示的数据为文件（纯前端）

```typescript
// 纯前端操作：ringBuffer → 序列化 → 下载 .ilr 文件
function saveFrontendData() {
  const features = dataBus.getAvailableFeatures()
  const lines: string[] = []

  // 文件头
  lines.push(JSON.stringify({
    format: 'ilr',
    version: 2,
    generated_at: Date.now(),
    generated_by: 'illuminator-web',
    features: features,
    window_seconds: selectedWindowSeconds,  // 30 | 60 | 120
  }))

  // 收集所有 Feature 的缓存数据
  for (const feature of features) {
    const recent = dataBus.getRecent(feature, selectedWindowSeconds * 2)
    for (const msg of recent) {
      lines.push(JSON.stringify(msg))
    }
  }

  // 下载
  const blob = new Blob([lines.join('\n') + '\n'], {
    type: 'application/x-illuminator-recording'
  })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `illuminator_${new Date().toISOString().replace(/[:.]/g, '-')}.ilr`
  a.click()
  URL.revokeObjectURL(url)
}
```

**时间窗口选择器**：

```tsx
function TimeWindowSelector() {
  const [window, setWindow] = useState(60) // 默认 1 分钟
  const options = [
    { label: '30 秒', value: 30 },
    { label: '1 分钟', value: 60 },
    { label: '2 分钟', value: 120 },
  ]

  return (
    <div className="time-window-selector">
      <label>时间窗口：</label>
      <select value={window} onChange={e => {
        const val = Number(e.target.value)
        setWindow(val)
        dataBus.setWindowSize(val)
      }}>
        {options.map(opt => (
          <option key={opt.value} value={opt.value}>{opt.label}</option>
        ))}
      </select>
      <button onClick={saveFrontendData}>导出当前数据</button>
    </div>
  )
}
```

### 5.2 功能 2：前端点击录制 → 后端文件 I/O 落盘

```cpp
// FeatureBus 提供录制控制接口
// 录制通过向 Pipeline 动态添加 RecordingSink 实现
Status FeatureBus::StartRecording(const std::string& name,
                                   const std::string& file_path) {
    auto feature = GetFeature(name);
    if (!feature) return Status::Error(StatusCode::kNotFound, "Feature not found");

    // 创建 RecordingSink 并添加到 Pipeline 的 Sink 列表
    auto rec_sink = std::make_shared<RecordingSink>(file_path);
    rec_sink->Init(/* config */);

    feature->AddSink(rec_sink);

    active_recordings_[name] = {file_path, rec_sink};
    return Status::Ok();
}

Status FeatureBus::StopRecording(const std::string& name) {
    auto it = active_recordings_.find(name);
    if (it == active_recordings_.end()) {
        return Status::Error(StatusCode::kNotFound, "No active recording");
    }

    auto feature = GetFeature(name);
    feature->RemoveSink(it->second.sink);

    it->second.sink->Flush();
    active_recordings_.erase(it);
    return Status::Ok();
}
```

**前端录制 UI**：

```tsx
function RecordingControl({ featureName }: { featureName: string }) {
  const [isRecording, setIsRecording] = useState(false)

  const startRecording = async () => {
    const filePath = `illuminator_${featureName}_${Date.now()}.ilr`
    await api.startRecording(featureName, filePath)
    setIsRecording(true)
  }

  const stopRecording = async () => {
    await api.stopRecording(featureName)
    setIsRecording(false)
  }

  return (
    <div className="recording-control">
      {isRecording ? (
        <button onClick={stopRecording} className="recording-active">
          <span className="recording-dot" /> 停止录制
        </button>
      ) : (
        <button onClick={startRecording}>开始录制</button>
      )}
    </div>
  )
}
```

### 5.3 前端配置面板（自动生成）

```tsx
// 根据 JSON Schema 自动生成配置表单
function FeatureConfigPanel({ featureName }: { featureName: string }) {
  const [schema, setSchema] = useState<JSONSchema | null>(null)
  const [config, setConfig] = useState<Record<string, unknown>>({})

  useEffect(() => {
    api.getConfigSchema(featureName).then(setSchema)
    api.getFeatureConfig(featureName).then(setConfig)
  }, [featureName])

  return (
    <div className="feature-config-panel">
      <h3>配置</h3>
      {/* 自动生成的配置表单 */}
      {schema && (
        <JsonSchemaForm
          schema={schema}
          value={config}
          onChange={setConfig}
        />
      )}

      <button onClick={() => api.setFeatureConfig(featureName, config)}>
        保存配置
      </button>
    </div>
  )
}
```

---

## 六、API 设计

### 6.1 控制面 (REST)

```
# Feature 元数据（前端自动发现）
GET  /api/v1/features
  → 返回所有 Feature 的 Descriptor，前端自动生成控制面板

# Feature 配置（前端自动生成配置表单）
GET  /api/v2/features                       → Feature 列表 {"features": [...descriptors]}
GET  /api/v2/features/:name/config/schema   → JSON Schema（配置表单定义）
GET  /api/v2/features/:name/config          → 当前配置值
POST /api/v2/features/:name/config          → 更新配置
GET  /api/v2/features/:name/stats           → 运行时统计

# Feature 生命周期 (v2 FeatureBus 原生)
POST /api/v2/features/:name/start           → FeatureBus::Probe()
POST /api/v2/features/:name/stop            → FeatureBus::Remove()
POST /api/v2/features/:name/pause           → FeatureBus::Pause()
POST /api/v2/features/:name/resume          → FeatureBus::Resume()

# 录制控制 (v1, 经 RecordingSinkRegistry)
POST /api/v1/features/:name/record/start    → RecordingSink::StartRecording()
  Body: { "file_path": "..." }  (可选，服务端自动生成路径)
POST /api/v1/features/:name/record/stop     → RecordingSink::StopRecording()
GET  /api/v1/features/:name/record/status   → { recording, file_path, bytes_written }

# 全局录制
POST /api/v1/recording/start                → 启动所有 feature 的录制
POST /api/v1/recording/stop                 → 停止所有 feature 的录制
GET  /api/v1/recording/status               → 所有 feature 的录制状态
```

### 6.2 数据面 (SSE — 订阅制)

```
# 步骤 1: 创建订阅（指定需要接收的 feature 列表）
POST /api/v1/events/subscribe
  Body: { "features": ["cpu_utilization", "process_cpu"] }
  → Response: { "subscription_id": "sub_0", "url": "/api/v1/events/sub_0" }

# 步骤 2: 建立 SSE 连接
GET /api/v1/events/:subscription_id
  → SSE 流 (Content-Type: text/event-stream)
  → 普通消息: event: data\ndata: {json}\n\n
  → 大消息分帧: event: frame\ndata: {frame_json}\n\n
  → 心跳保活: : keepalive\n\n (每 15s)

# 步骤 3: 动态更新订阅 (可选)
POST /api/v1/events/:subscription_id/update
  Body: { "add": ["io_monitor"], "remove": ["process_cpu"] }
```

### 6.3 DataBatch 序列化格式

```json
{
  "feature": "cpu_utilization",
  "modelType": "time_series",
  "timestamp": 1719900000000,
  "seq": 1234,
  "metrics": [
    {
      "labels": {"cpu": "cpu0", "source": "cpu_utilization", "type": "cpu_core"},
      "fields": {"busy_pct": 45.2, "user_pct": 30.1, "system_pct": 15.1},
      "timestamp": 1719900000000
    }
  ]
}
```

**注意**：`time_series` 类型使用 `"metrics"` 字段，`trace`/`generic` 类型使用 `"records"` 字段。
前端 `extractRecords()` 工具函数统一兼容两种格式。

#### 6.3.1 Frame 分帧格式 (当 JSON > 64KB 时)

```json
{
  "feature": "cpu_profiler",
  "seq": 42,
  "frame_idx": 0,
  "frame_total": 3,
  "payload": "<json_chunk_base64_or_raw>"
}
```
```

---

## 七、配置变更

### YAML 配置

```yaml
# 新配置（增加 feature_type 字段，Meta 从 Descriptor 获取）
features:
  cpu_utilization:
    feature_type: cpu_utilization    # ← 匹配 FeatureDriver 类型
    source:
      type: cpu_utilization
      config:
        interval_ms: 1000
    sinks:
      - type: local_storage

  cpu_profiler:
    feature_type: cpu_profiler
    source:
      type: ebpf_cpu_profiler
      config:
        frequency_hz: 49
    processors:
      - type: stack_symbolizer
      - type: stack_merger
    sinks:
      - type: local_storage

# 基础设施配置
infrastructure:
  collect_pool:
    threads: 4
  critical_sink_pool:       # 关键通道（SSE 推送）
    threads: 2
  noncritical_sink_pool:    # 非关键通道（文件 I/O）
    threads: 4

# 全局配置
always_on:
  auto_start_tier_1_2: true

server:
  http:
    enabled: true
    port: 8080
  # websocket 配置已移除
```

---

## 八、Feature 自动注册宏

```cpp
#define REGISTER_FEATURE(FeatureClass) \
    static const bool _##FeatureClass##_feature_registered = []() { \
        FeatureBus::Instance().RegisterFeatureType( \
            std::make_unique<FeatureClass>()); \
        return true; \
    }()
```

---

## 九、迁移计划

### Phase 1: 基础设施拆分 (4 小时)

1. 创建 `InfrastructureManager`，单 SinkPool（队列满丢弃）
2. 修改 `PipelineController` 内部使用 `InfrastructureManager`
3. 验证：现有测试全部通过

### Phase 2: FeatureDriver 基类 + CpuUtilizationDriver (6 小时)

1. 创建 `FeatureDriver` 基类、`FeatureDescriptor`、`FeatureParams`
2. 实现 `CpuUtilizationDriver` 作为第一个 Feature
3. 创建 `FeatureBus`
4. 修改 `main.cc` 使用 `FeatureBus` + `CpuUtilizationDriver`
5. 验证：daemon 启动，Web UI 显示 CPU 数据

### Phase 3: 迁移所有 Feature (8 小时)

1. 逐个实现所有 FeatureDriver
2. 修改 `main.cc` 删除硬编码的 lambda 函数
3. 验证：所有 Feature 在 Web UI 中正确显示和控制

### Phase 4: 数据面优化 (4 小时)

1. 实现 `SseHandler`，替代 `WebSocketManager`
2. 实现前端 `DataBus`，替代 `LiveDataSource` Link Chain
3. 移除 `StreamSinkStore`（Buffer 不再需要）
4. 验证：推送延迟 < 100ms，所有测试通过

### Phase 5: 废弃旧类 (4 小时)

1. 删除 `FeatureManager` 类
2. 删除 `PipelineController` 类（保留 `Pipeline` 类）
3. 删除 `WebSocketManager`、`WebSocketServer`
4. 删除 `StreamSinkStore`
5. 清理 `api_routes.h` 中对旧类的引用
6. 验证：编译通过，所有测试通过

### Phase 6: 前端功能 (6 小时)

1. 实现前端配置面板（JSON Schema 自动生成表单）
2. 实现前端保存数据（ringBuffer 序列化下载）
3. 实现前端录制控制（按钮 → 后端文件 I/O）
4. 实现时间窗口选择器
5. 验证：新增 Feature 时前端零改动

### 总工作量

| Phase | 内容 | 工作量 |
|-------|------|--------|
| Phase 1 | 基础设施拆分 | 4 小时 |
| Phase 2 | FeatureDriver 基类 + 第一个 Feature | 6 小时 |
| Phase 3 | 迁移所有 Feature | 8 小时 |
| Phase 4 | 数据面优化（SSE 替代） | 4 小时 |
| Phase 5 | 废弃旧类 | 4 小时 |
| Phase 6 | 前端功能 | 6 小时 |
| **总计** | | **~32 小时** |

---

## 十、成功指标

| 指标 | 当前 | 目标 |
|------|------|------|
| FeatureManager 行数 | 1200 | 0（废弃） |
| PipelineController 行数 | 600 | 0（废弃） |
| StreamSinkStore 行数 | 340 | 0（废弃） |
| WebSocketManager 行数 | 600 | 0（废弃） |
| InfrastructureManager 行数 | 0 | 80 |
| FeatureBus 行数 | 0 | 200 |
| SseHandler 行数 | 0 | 150 |
| 单个 FeatureDriver 行数 | 混在 FeatureManager 中 | 50-70 |
| 新增 Feature 需修改的文件 | 3 个 | 1 个 |
| 新增 Feature 前端改动 | 改 3 处 | 0 处（自动发现） |
| main.cc 硬编码映射 | 3 个 lambda | 0 |
| 推送延迟 | 0-1000ms | < 100ms |
| 上帝类文件 | 3 个 | 0 个 |
| 总代码行数 | ~3360 | ~880 |

---

## 十一、架构对比

```
【当前架构】
  main.cc (硬编码映射)
    ├── PipelineController (600 行)
    │   ├── TimerWheel + CollectPool + SinkPool
    │   ├── BuildFromConfig
    │   └── Pipelines[]
    ├── FeatureManager (1200 行)
    │   ├── CreateAndStartPipeline (10 件事)
    │   ├── 生命周期 + 状态机
    │   └── 注入 StreamSink + RecordingSink
    ├── StreamSinkStore (340 行)
    │   └── Ring Buffer + PollSince
    ├── WebSocketManager (600 行)
    │   └── BroadcastLoop (1 秒轮询)
    └── api_routes.h (1200 行)

【新架构】
  main.cc (极简，只注册和启动)
    ├── InfrastructureManager (80 行)
    │   ├── TimerWheel
    │   ├── CollectPool
    │   └── SinkPool (队列满丢弃，kMaxPendingTasks = 256)
    ├── FeatureBus (200 行)
    │   ├── Feature 类型注册表
    │   ├── Feature 实例注册表
    │   ├── 生命周期编排
    │   └── SSE 连接管理
    ├── FeatureDriver 实现 (10 个 × 60 行 = 600 行)
    │   ├── CpuUtilizationDriver
    │   ├── CpuProfilerDriver
    │   └── ...
    ├── SseHandler (150 行)
    │   └── 条件变量通知 + SSE 推送
    └── 路由文件 (3 个 × 150 行)
        ├── core_routes.h
        ├── feature_routes.h
        └── deprecated_routes.h
```

---

## 十二、为什么这个设计是"优秀"的

### 1. 职责单一

每个类只做一件事：
- `InfrastructureManager`：只管线程池和定时器
- `FeatureBus`：只管 Feature 注册和编排
- `FeatureDriver` 子类：只管自己的数据采集逻辑
- `SseHandler`：只管 SSE 推送

### 2. 开放-封闭原则

新增 Feature 不需要修改框架代码：
- 框架层对扩展开放
- `REGISTER_FEATURE` 宏自动注册
- 新增 Feature 只需写一个 `FeatureDriver` 子类

### 3. 依赖倒置

- `FeatureBus` 依赖 `FeatureDriver` 抽象接口，不依赖具体 Feature
- 具体 Feature 通过 `FeatureBus` 访问基础设施
- 上层（API 路由）依赖 `FeatureBus` 抽象

### 4. 自描述

- `FeatureDescriptor` 让前端自动发现 Feature 能力和参数
- JSON Schema 让前端自动生成配置表单
- 新增 Feature 时前端零改动

### 5. 可测试

- 每个 `FeatureDriver` 可以独立测试
- `FeatureBus` 可以 mock
- `SseHandler` 可以独立测试

### 6. 极简数据流

- SSE 直达，无 Buffer 中间层
- 条件变量通知，延迟 < 100ms
- 代码量减少 87%（1560 → 210 行）

### 7. 线程安全

- SinkPool 线程只做内存操作（push + notify），不写 fd
- 写 fd 的是 HTTP 请求处理线程，慢客户端不阻塞 SinkPool

---

## 附录 A：设计决策记录

### A.1 为什么移除 Buffer？

| 场景 | 需要 Buffer 吗？ | 决策 |
|------|-----------------|------|
| 实时推送 | 不需要 | 直接 SSE 推送 |
| 前端重连 | 不需要 | 重连后从头开始接收 |
| 页面刷新 | 不需要 | 同上 |
| 导出 | 不需要 | 纯前端 ringBuffer 序列化下载 |
| 录制 | 不需要 | 从此刻开始录制 |
| 多标签页 | 不需要 | 新标签页各自独立 |

**仅有导出超长时间范围数据（> 前端窗口）时需要 Buffer，这是高级功能，非核心需求。**

### A.2 为什么 SSE 替代 WebSocket？

| 维度 | WebSocket | SSE |
|------|----------|-----|
| 方向 | 双向 | 单向（服务器→客户端） |
| 协议 | 自定义 | HTTP 标准 |
| 浏览器支持 | 需要 JS 库 | 原生 EventSource API |
| 自动重连 | 需手动实现 | 内置 |
| 代理兼容 | 需要升级 | 标准 HTTP |
| Illuminator 需求 | 只需要推送 | 只需要推送 |

### A.3 为什么 SSE 推送不在 SinkPool 中完成？

SinkPool 线程只做快速内存操作（push 到队列 + 条件变量通知），不写 fd。写 fd 的是 httplib 为每个 SSE 连接创建的 HTTP 请求处理线程。即使客户端 TCP 缓冲区满了，`sink.write()` 阻塞，阻塞的也是 HTTP 线程，SinkPool 线程完全不受影响。因此不需要分离 SinkPool 来保护 SSE 推送。

### A.4 与 Linux 驱动模型的一致性

- 业界有大量参考资料和最佳实践
- 新开发者容易理解和上手
- 设计模式成熟，经过大规模验证

---

## §11 实现偏差记录 (Implementation Deviations)

> 本节记录实际实现与原始设计方案之间的有意偏差。  
> 所有偏差均为实现过程中的优化决策，保持核心三层架构不变。

### 11.1 命名变更

| 原设计 | 实际实现 | 原因 |
|--------|---------|------|
| `FeatureDriver::OnStart(params)` | `FeatureDriver::Probe()` | 更贴合 Linux 驱动 `probe/remove` 语义 |
| `FeatureDriver::OnStop()` | `FeatureDriver::Remove()` | 同上 |
| `FeatureBus::StartFeature(name)` | `FeatureBus::Probe(name)` | API 一致性 |
| `FeatureBus::StopFeature(name)` | `FeatureBus::Remove(name)` | API 一致性 |
| `FeatureBus::ListFeatures()` | `FeatureBus::ListDrivers()` + `ListDescriptors()` | 分离元数据和运行信息 |
| `FeatureState` / `FeatureTier` | `DriverState` / `DriverTier` | Driver-centric 命名 |
| `StreamSink` → `SseHandler::Notify()` | `SseSink` → `SseHandler::Publish()` | 明确职责 |

### 11.2 架构增强

| 项目 | 原设计 | 实际实现 | 优势 |
|------|--------|---------|------|
| **SSE 连接模型** | 单一 `GET /api/v1/events` 全局流 | 订阅制：`POST /subscribe` → `GET /events/{id}` → `POST /{id}/update` | 支持按需订阅过滤，减少无用数据传输 |
| **SSE 大消息** | 未考虑 | Frame splitting（64KB 分帧 + `seq/frame_idx/frame_total` 重组） | 避免大 JSON 阻塞连接 |
| **SSE Keepalive** | 依赖 SSE 内置重连 | `: keepalive\n\n` 每 15s | 防止代理/LB 超时断开 |
| **前端 ringBuffer** | 全局单 buffer，60 条 | Per-feature 独立 buffer，默认 300 条 | 每个 feature 独立时间窗口 |
| **REGISTER_FEATURE** | 直接注册到 FeatureBus | 两阶段：`FeatureRegistry` → `RegisterAll()` → `FeatureBus` | 支持 CLI 一次性命令（不启动 FeatureBus） |
| **FeatureDriver::BuildPipeline** | 嵌入 OnStart() | 独立纯虚方法 `BuildPipeline(InfrastructureManager&)` | 关注点分离，base class 统一 timer/sink 注册 |

### 11.3 RecordingSink 策略变更

| 原设计 | 实际实现 |
|--------|---------|
| 用户点录制时动态创建 RecordingSink 并添加到 Pipeline | RecordingSink 在 `Probe()` 时始终附加到 Pipeline |
| `FeatureBus::StartRecording(name, file_path)` 控制 | `RecordingSinkRegistry::Get(name)->StartRecording()` 经 REST 控制 |
| 客户端指定 `file_path` | 服务端自动生成 `{feature}_{epoch}.ilr` 于 `/tmp/illuminator_recordings/` |

**决策依据**：始终附加 + 非录制时不写 IO 的设计更简单，避免运行时动态修改 Pipeline 的线程安全问题。

### 11.4 API 路径版本

| 原设计 | 实际实现 | 说明 |
|--------|---------|------|
| `/api/v1/features/*` | `/api/v2/features/*` | v1 路径用于旧式兼容（deprecated header），v2 为 FeatureBus 原生 |
| `/api/v1/sessions` | 未实现 | Tier 3 session 管理通过 `/api/v2/features/:name/start` + config body 替代 |
| `/api/v1/export` | 未实现（后续） | 前端 ringBuffer 导出已满足当前需求 |

### 11.5 PipelineController 已完全移除

`PipelineController` 类已被删除。`Pipeline` 类独立定义在 `src/core/engine/pipeline.h` 中，
由 `FeatureDriver` 通过 `BuildPipeline()` 创建并持有（`std::unique_ptr<Pipeline> pipeline_`）。

CLI `illuminator collect` 命令同样使用 `FeatureBus::ProbeAll()` → sleep → `FeatureBus::RemoveAll()`，
与 daemon 模式走完全相同的数据路径。

### 11.6 文件组织

| 原设计路径 | 实际路径 |
|-----------|---------|
| `src/core/infrastructure/infrastructure_manager.h` | `src/core/engine/infrastructure_manager.h` |
| `src/core/bus/feature_bus.h` | `src/core/engine/feature_bus.h` |
| `src/core/driver/feature_driver.h` | `src/core/engine/feature_driver.h` |
| (Pipeline 嵌入 PipelineController) | `src/core/engine/pipeline.h`（独立文件） |
| `src/plugin/features/cpu_utilization_driver.h` | `src/plugin/features/cpu_utilization_driver.h` ✅ |
| `src/server/sse_handler.h` | `src/server/sse_handler.h` ✅ |

### 11.7 已实现的 7 个 FeatureDriver

| Driver | Name | Tier | 状态 |
|--------|------|------|------|
| `CpuUtilizationDriver` | `cpu_utilization` | Monitoring | Active (auto-probe) |
| `ProcessCpuDriver` | `process_cpu` | Monitoring | Active (auto-probe) |
| `CpuProfilerDriver` | `cpu_profiler` | Profiling | Inactive (需 eBPF 对象) |
| `IoMonitorDriver` | `io_monitor` | Tracing | Active (idle mode) |
| `NetTracerDriver` | `net_tracer` | Tracing | Active (idle mode) |
| `SchedAnalyzerDriver` | `sched_analyzer` | Tracing | Active (idle mode) |
| `OffcpuProfilerDriver` | `offcpu_profiler` | Profiling | Inactive (需 eBPF 对象) |

### 11.8 前端实现细节

| RFC 描述 | 实际实现 |
|---------|---------|
| `DataBus` ~60 行 | `DataBus` (278行) + `SseLink` (92行) = ~370 行 |
| `new EventSource('/api/v1/events')` 直连 | `POST /subscribe` + `EventSource(url)` + `POST /update` 动态管理 |
| `onmessage` 处理所有事件 | `addEventListener('data')` + `addEventListener('frame')` 分类处理 |
| SSE payload: `batch.data.records` | SSE payload: `batch.data.metrics` (time_series) 或 `batch.data.records` (trace/generic) |
| 前端使用 `extractRecords()` 工具函数统一适配两种格式 | 见 `web/src/utils/ssePayload.ts` |