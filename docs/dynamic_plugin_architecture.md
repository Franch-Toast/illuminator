# Illuminator 动态插件架构设计 v2 — 按需启停 · 实时流 · 录制回放

> **版本**: 2.2  
> **状态**: ⚠️ **已过时 (Deprecated)** — 本文档描述基于 `FeatureManager` + `WebSocket` 的旧架构。  
> **当前架构**: RFC v3 三层架构（`InfrastructureManager` + `FeatureBus` + `FeatureDriver`），SSE 替代 WebSocket。  
> **参考文档**: `docs/rfc_data_contract_v2.md`（RFC v3 设计）、`docs/onboarding_guide.md`（入门指南）  
> **保留原因**: 作为历史参考，Tier 分层交互理念在新架构中仍适用。  
> **核心交互模型**: 分层自动激活（Tier 1/2 自动运行，Tier 3 手动触发）

---

## 一、核心体验模型

### 1.1 用户视角

```
┌─────────────────────────────────────────────────────────────────┐
│  用户打开 Illuminator Web UI                                      │
│                                                                  │
│  每个功能标签页（CPU Overview / Off-CPU / Scheduler / ...）：      │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  ┌─────────────────────────────────────────────────────┐ │    │
│  │  │  CPU Utilization              [启动] [录制]         │ │    │
│  │  │                                                     │ │    │
│  │  │      ┌────────── 图表区域 ──────────┐               │ │    │
│  │  │      │                              │               │ │    │
│  │  │      │    "功能未启动"               │               │ │    │
│  │  │      │    点击启动按钮开始采集        │               │ │    │
│  │  │      │                              │               │ │    │
│  │  │      └──────────────────────────────┘               │ │    │
│  │  └─────────────────────────────────────────────────────┘ │    │
│  │                                                           │    │
│  │  用户点击 [启动] 后 →                                      │    │
│  │                                                           │    │
│  │  ┌─────────────────────────────────────────────────────┐ │    │
│  │  │  CPU Utilization              [暂停] [录制]         │ │    │
│  │  │                                                     │ │    │
│  │  │      ┌────────── 实时曲线 ──────────┐               │ │    │
│  │  │      │  ___/\___/\___/\___/\___     │               │ │    │
│  │  │      │  <--- 60s 滑动窗口 --->      │               │ │    │
│  │  │      └──────────────────────────────┘               │ │    │
│  │  └─────────────────────────────────────────────────────┘ │    │
│  └──────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 状态模型（极简三态）

每个功能面板只有三个用户可感知的状态：

```
         [启动]                [暂停]               [恢复]
Inactive ──────────> Active ──────────> Paused ──────────> Active
    ^                   |                                    |
    +─────── [停止] <───+────────────────────────────────────+
```

| 状态 | UI 表现 | 后端行为 |
|------|---------|---------|
| **Inactive** | 空图表 + "未启动" 提示 + 启动按钮 | 无 pipeline 实例 |
| **Active** | 实时数据流入 + 图表动态更新 | Pipeline 运行中，WS 推送 |
| **Paused** | 图表冻结在最后帧 + "已暂停" 标记 | Pipeline 暂停采集 |

### 1.3 录制是 Active 状态的叠加层

录制不是独立状态，而是 Active 状态上的一个附加行为：

```
Active + Recording OFF  ->  数据只流向前端（实时展示）
Active + Recording ON   ->  数据同时流向前端 + 磁盘文件

关系: Recording 是 Active 的子集（只有 Active 时才能录制）
```

---

## 二、系统架构

### 2.1 数据流全景

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│  ┌──────────────┐                                                           │
│  │ eBPF/procfs  │  <- 内核/系统                                              │
│  └──────┬───────┘                                                           │
│         | 原始事件                                                           │
│         v                                                                   │
│  ┌──────────────────────────────────────────────────────────────┐           │
│  │              Pipeline (按需创建/销毁)                          │           │
│  │                                                              │           │
│  │  Source -> [Processors] -> Fanout                            │           │
│  │                            |                                 │           │
│  │              +─────────────+──────────────+                  │           │
│  │              |             |              |                  │           │
│  │              v             v              v                  │           │
│  │     ┌──────────────┐ ┌─────────┐ ┌────────────┐            │           │
│  │     │ WS Stream    │ │ Ring    │ │ Recording  │ (可选)      │           │
│  │     │ (实时推送)    │ │ Buffer  │ │ Sink       │            │           │
│  │     └──────┬───────┘ │ (快照)  │ └─────┬──────┘            │           │
│  │            |          └────┬────┘       |                   │           │
│  └────────────|───────────────|────────────|───────────────────┘           │
│               |               |            |                                │
│               v               v            v                                │
│  ┌──────────────────┐  ┌──────────┐  ┌──────────────────┐                  │
│  │ Frontend         │  │ HTTP API │  │ File System      │                  │
│  │ (WebSocket RX)   │  │ (GET)    │  │ /data/recordings/│                  │
│  └──────────────────┘  └──────────┘  └──────────────────┘                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 实时数据通道 | WebSocket push | 比 HTTP polling 延迟低 10-100x，无冗余请求 |
| 前端数据管理 | 客户端 Ring Buffer | 服务端无需为每个前端维护历史，内存可控 |
| 录制格式 | 自描述二进制 (Header + Records) | 支持快速 seek 回放，无 JSON 解析开销 |
| Pipeline 粒度 | 功能 = Pipeline = 前端面板 | 1:1 映射，概念清晰，独立热插拔 |
| 录制与实时解耦 | Fanout (zero-copy shared_ptr) | 录制不影响实时路径性能 |

### 2.3 组件职责（无冗余原则）

```
┌─────────────────────────────────────────────────────────────────┐
│ 后端组件                                                         │
├────────────────────┬────────────────────────────────────────────┤
│ FeatureManager     │ 统一入口：管理所有"功能"的生命周期            │
│                    │ 一个"功能" = 一个 pipeline + 关联的前端面板    │
│                    │ 替代原 PipelineController 的启停逻辑         │
├────────────────────┼────────────────────────────────────────────┤
│ Pipeline (v3)      │ 数据处理引擎，不变                           │
│                    │ Source -> Processors -> Sinks              │
├────────────────────┼────────────────────────────────────────────┤
│ StreamSink         │ 将 DataBatch 序列化并推送到 WebSocket        │
│                    │ 替代原 websocket_sink（更轻量）              │
├────────────────────┼────────────────────────────────────────────┤
│ RecordingSink      │ 将 DataBatch 写入录制文件                   │
│                    │ 按需附着/分离，不随 pipeline 常驻            │
├────────────────────┼────────────────────────────────────────────┤
│ ReplayEngine       │ 读取录制文件，模拟实时推送到前端              │
│                    │ 支持 seek/speed/pause                      │
└────────────────────┴────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 前端组件                                                         │
├────────────────────┬────────────────────────────────────────────┤
│ FeaturePanel       │ 通用面板壳：标题 + 状态指示 + 控制按钮        │
│                    │ 包裹具体的可视化组件                          │
├────────────────────┼────────────────────────────────────────────┤
│ TimeSeriesBuffer   │ 客户端 Ring Buffer，固定窗口滑动             │
│                    │ 自动淘汰超出时间窗口的旧数据                  │
├────────────────────┼────────────────────────────────────────────┤
│ StreamReceiver     │ WebSocket 客户端，按 feature 订阅            │
│                    │ 收到数据 -> 写入 TimeSeriesBuffer            │
├────────────────────┼────────────────────────────────────────────┤
│ RecordingControl   │ 录制状态管理 + 进度显示                      │
│                    │ 调用后端 API 启停录制                        │
└────────────────────┴────────────────────────────────────────────┘
```

---

## 三、Feature 概念——前端面板与后端 Pipeline 的 1:1 映射

### 3.1 Feature 定义

一个 **Feature** 是面向用户的最小可操作单元：

```yaml
features:
  cpu_utilization:
    display_name: "CPU Utilization"
    icon: "cpu"
    category: "cpu"
    pipeline:
      source: { type: cpu_utilization, config: { interval_ms: 1000 } }
      processors: []
      sinks: []  # StreamSink 自动注入，无需手动配置
    frontend:
      type: "timeseries"         # 时序曲线
      window_sec: 60             # Ring Buffer 保留 60 秒
      refresh_interval_ms: 1000  # 图表刷新频率

  offcpu_profile:
    display_name: "Off-CPU Profile"
    icon: "flame"
    category: "profiling"
    pipeline:
      source:
        type: offcpu_profiler
        config:
          min_duration_us: 1000
          target_comms: ["illuminator"]
      processors: [{ type: stack_symbolizer }]
    frontend:
      type: "flamegraph"
      window_sec: 30             # 保留最近 30 秒的采样

  sched_analysis:
    display_name: "Scheduler Timeline"
    icon: "timeline"
    category: "scheduler"
    pipeline:
      source: { type: sched_analyzer }
      processors: []
    frontend:
      type: "timeline"
      window_sec: 10
```

### 3.2 Feature 与 Pipeline 的关系

```
Feature (面向用户)          Pipeline (面向引擎)
─────────────────           ──────────────────
display_name               name
状态 (inactive/active)     状态 (stopped/running/paused)
控制按钮                   Start()/Stop()/Pause()
录制控制                   AttachSink(RecordingSink)
前端参数                   无（前端自行管理）
```

**为什么 1:1？**
- 用户心智模型清晰：一个按钮控制一个采集功能
- 无需理解 pipeline/source/sink 等内部概念
- 避免多对多映射的复杂性

### 3.3 是否还需要 YAML 配置文件？

**结论：仅需一个声明式配置，定义可用 Feature 集合。**

```yaml
# illuminator.yaml — 唯一配置文件
# 作用：声明这个实例"有能力"运行哪些 feature
# 不决定哪些 feature 实际运行（那是用户在前端决定的事）

global:
  log_level: info
  data_dir: /var/lib/illuminator

server:
  listen: "0.0.0.0:9527"

features:
  cpu_utilization:
    # ... (如上)
  offcpu_profile:
    # ...
  sched_analysis:
    # ...
  cpu_profiler:
    # ...

recording:
  dir: /var/lib/illuminator/recordings
  max_file_size_mb: 500       # 单文件上限
  max_total_size_mb: 2048     # 总磁盘上限
  auto_cleanup_days: 7        # 自动清理
```

**不需要 `auto_start`**。所有 Feature 默认 Inactive，用户决定启动哪些。如果需要"开机自启"语义（如无人值守部署），可加：

```yaml
# 可选：部署模式 — 适用于 headless/CI 场景
deploy_mode:
  auto_start: ["cpu_utilization", "sched_analysis"]
```

---

## 四、前端 Ring Buffer 与数据流

### 4.1 TimeSeriesBuffer 设计

```typescript
class TimeSeriesBuffer<T> {
  private buffer: T[] = [];
  private readonly windowMs: number;

  constructor(windowSec: number) {
    this.windowMs = windowSec * 1000;
  }

  push(item: T & { timestamp: number }): void {
    this.buffer.push(item);
    this.evict();
  }

  private evict(): void {
    const cutoff = Date.now() - this.windowMs;
    let lo = 0, hi = this.buffer.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (this.buffer[mid].timestamp < cutoff) lo = mid + 1;
      else hi = mid;
    }
    if (lo > 0) this.buffer.splice(0, lo);
  }

  getAll(): T[] { return this.buffer; }
  getLast(n: number): T[] { return this.buffer.slice(-n); }
  get length(): number { return this.buffer.length; }
  clear(): void { this.buffer = []; }
}
```

**性能特性**:
- Push: O(1) 摊还（evict 使用二分查找 O(log n) + splice）
- 内存稳定：永远不超过 `windowSec x 数据频率` 条记录
- 典型大小：CPU 利用率 @ 1Hz x 60s = 60 条；Sched events @ 1000Hz x 10s = 10K 条

### 4.2 前端数据订阅流程

```typescript
// useFeatureStream hook
function useFeatureStream(featureName: string) {
  const [state, setState] = useState<'inactive' | 'active' | 'paused'>('inactive');
  const buffer = useRef(new TimeSeriesBuffer(windowSec));
  const ws = useRef<WebSocket | null>(null);

  const start = async () => {
    // 1. 调后端 API 启动 pipeline
    await api.post(`/features/${featureName}/start`);
    // 2. 建立 WebSocket 订阅
    ws.current = new WebSocket(`${wsHost}/ws/stream/${featureName}`);
    ws.current.onmessage = (e) => {
      const batch = deserialize(e.data);
      buffer.current.push(batch);
    };
    setState('active');
  };

  const stop = async () => {
    ws.current?.close();
    await api.post(`/features/${featureName}/stop`);
    buffer.current.clear();
    setState('inactive');
  };

  return { state, buffer: buffer.current, start, stop, pause, resume };
}
```

### 4.3 数据推送协议

WebSocket 采用二进制帧（MessagePack），避免 JSON 序列化开销：

```
┌─────────────────────────────────────────────────┐
│ WebSocket Binary Frame                           │
├────────┬──────────┬────────────────────────────┤
│ Header │ 8 bytes  │ feature_id(2) + seq(4) +   │
│        │          │ record_count(2)             │
├────────┼──────────┼────────────────────────────┤
│ Body   │ variable │ MessagePack encoded records │
└────────┴──────────┴────────────────────────────┘
```

对比各序列化方案：

| 方案 | 编码速度 | 体积 | 前端解析 | 选择理由 |
|------|---------|------|---------|---------|
| JSON | 慢 | 大 | 原生 | 当前方案，开发简单 |
| MessagePack | 快 | 小 40% | msgpack-lite | 推荐：平衡性能与开发效率 |
| FlatBuffers | 最快 | 最小 | 需 schema | 过度工程化 |
| Protobuf | 快 | 小 | 需编译 | 依赖重 |

**Phase 1 用 JSON，Phase 2 迁移到 MessagePack**（前端加一个 `deserialize` 层即可切换）。

---

## 五、录制与回放

### 5.1 录制文件格式

设计原则：自描述、可 seek、支持增量写入、支持快速回放。

```
┌─────────────────────────────────────────────────────────────────┐
│                  Illuminator Recording File (.ilr)                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  +─────────────── File Header (固定 256 bytes) ───────────────+  │
│  | magic: "ILR\x01"           (4 bytes)                       |  │
│  | version: 1                  (2 bytes)                       |  │
│  | feature_name: padded        (64 bytes, null-padded)        |  │
│  | created_at: unix_ns         (8 bytes)                       |  │
│  | source_config: msgpack      (128 bytes, zero-padded)       |  │
│  | schema_hash: crc32          (4 bytes)                       |  │
│  | reserved                    (46 bytes)                      |  │
│  +────────────────────────────────────────────────────────────+  │
│                                                                  │
│  +─────────────── Record Blocks ─────────────────────────────+   │
│  |                                                            |   │
│  |  Record 0:                                                 |   │
│  |  +─────────────────────────────────────────────────────+   |   │
│  |  | timestamp_ns (8) | payload_len (4) | payload (var)  |   |   │
│  |  +─────────────────────────────────────────────────────+   |   │
│  |                                                            |   │
│  |  Record 1: ...                                             |   │
│  |  Record N: ...                                             |   │
│  |                                                            |   │
│  +────────────────────────────────────────────────────────────+  │
│                                                                  │
│  +─────────────── Index Block (尾部) ────────────────────────+   │
│  | 每 1000 条记录一个索引条目，用于快速 seek                    |   │
│  | [timestamp_ns, file_offset] x M                            |   │
│  +────────────────────────────────────────────────────────────+  │
│                                                                  │
│  +─────────────── Footer (16 bytes) ─────────────────────────+   │
│  | total_records (8) | index_offset (8)                       |   │
│  +────────────────────────────────────────────────────────────+  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**为什么不直接用 JSONL？**
- JSONL 无法 seek（必须顺序扫描全文件）
- 回放时需要按时间跳转
- 二进制格式体积小 3-5x，写入速度快

**为什么每个 Feature 单独一个文件？**
- 不同 Feature 数据频率/schema 不同，混合存储需要复杂的分路逻辑
- 单文件可独立上传/下载/删除
- 磁盘限制可按 Feature 分别控制

### 5.2 录制控制流

```
用户点击 [录制]
     |
     v
  POST /api/v1/features/{name}/record/start
  Body: { "max_duration_sec": 300, "max_size_mb": 200 }

  后端:
  1. 创建 RecordingSink 实例
  2. 打开文件: /data/recordings/{feature}_{timestamp}.ilr
  3. 写入 File Header
  4. 将 RecordingSink 热附着到 Pipeline 的 fanout
  5. 返回 recording_id

  数据流分叉：
  DataBatch -> Fanout --+--> StreamSink (实时) --> WebSocket --> 前端
                        |
                        +--> RecordingSink (录制) --> .ilr 文件

  (用户点击 [停止录制] 或达到限制)

  POST /api/v1/features/{name}/record/stop

  后端:
  1. 将 RecordingSink 从 fanout 分离
  2. 写入 Index Block + Footer
  3. 关闭文件
  4. 注册到 RecordingStore（metadata 索引）
```

### 5.3 回放机制

```
用户上传 .ilr 文件 或 选择历史录制
     |
     v
  POST /api/v1/replay/load
  Body: multipart/form-data (file upload)
  或: { "recording_id": "xxx" }

  后端:
  1. 解析 File Header -> 确定 feature_name + schema
  2. 读取 Index Block -> 构建时间索引
  3. 创建 ReplaySession
  4. 返回 session_id + metadata (duration, records, size)

  前端切换到"回放模式"
  同一个图表组件，数据源从 live WebSocket 切换为 replay WebSocket

  WebSocket: /ws/replay/{session_id}

  控制命令 (client -> server):
    { "cmd": "play", "speed": 1.0 }
    { "cmd": "pause" }
    { "cmd": "seek", "timestamp_ns": 1718100000000000000 }
    { "cmd": "speed", "multiplier": 2.0 }

  数据推送 (server -> client):
    与实时模式相同的帧格式
    ReplayEngine 按原始时间戳间距 / speed 推送
```

### 5.4 前端回放 UI

```
┌─────────────────────────────────────────────────────────────────┐
│  CPU Utilization  [回放模式]                           [X 退出]   │
│                                                                 │
│  +──────────────── 时序曲线（与实时模式相同） ──────────────+      │
│  |  ___/\___/\___/\___/\___/\___/\___/\___/\___/\___      |      │
│  +───────────────────────────────────────────────────────+      │
│                                                                 │
│  +──────────────── 回放控制条 ────────────────────────────+      │
│  |  [<<] [Play] [>>]   01:23 / 05:00   Speed: [1x]       |      │
│  |  |──────────X──────────────────────────────────────────|      │
│  |  0:00                                              5:00|      │
│  +───────────────────────────────────────────────────────+      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**关键设计**: 前端可视化组件**不感知**数据来源是实时还是回放。统一的数据接口：

```typescript
interface DataSource<T = DataBatch> {
  readonly mode: 'live' | 'replay';
  subscribe(callback: (batch: T) => void): () => void;
}

class LiveDataSource<T> implements DataSource<T> {
  readonly mode = 'live';
  // WebSocket live stream
}

class ReplayDataSource<T> implements DataSource<T> {
  readonly mode = 'replay';
  // WebSocket replay stream + controls (play/pause/seek/speed)
}
```

---

## 六、后端核心组件

### 6.1 FeatureManager — 统一控制入口

```cpp
class FeatureManager {
public:
    enum class FeatureState { kInactive, kStarting, kActive, kPaused, kStopping };

    struct Feature {
        std::string name;
        std::string display_name;
        FeatureState state = FeatureState::kInactive;
        PipelineConfig pipeline_config;
        std::unique_ptr<Pipeline> pipeline;

        // 录制
        bool is_recording = false;
        std::string recording_id;
        std::shared_ptr<RecordingSink> recording_sink;
    };

    // 生命周期
    Status Start(const std::string& feature);
    Status Stop(const std::string& feature);
    Status Pause(const std::string& feature);
    Status Resume(const std::string& feature);

    // 录制
    StatusOr<std::string> StartRecording(const std::string& feature,
                                          const RecordingParams& params);
    Status StopRecording(const std::string& feature);

    // 查询
    std::vector<FeatureInfo> ListFeatures() const;

private:
    StatusOr<std::unique_ptr<Pipeline>> CreatePipeline(const PipelineConfig& cfg);

    Status AttachSinkToPipeline(Pipeline* p, const std::string& sink_id,
                                std::shared_ptr<SinkPlugin> sink);
    Status DetachSinkFromPipeline(Pipeline* p, const std::string& sink_id);

    std::unordered_map<std::string, Feature> features_;
    mutable std::shared_mutex mutex_;
};
```

**相比 v1 方案的简化**:
- 去掉了 ConfigStore 三层合并 -> 单文件 YAML 足够
- 去掉了独立 LifecycleManager -> FeatureManager 内聚管理
- 去掉了 7 态 FSM -> 3 种用户可感知状态 + 2 种过渡态
- 去掉了 user_overrides.yaml -> 无需持久化运行时状态

### 6.2 StreamSink — 零拷贝 WebSocket 推送

替代当前的 `websocket_sink`（有不必要的中间 buffer）：

```cpp
class StreamSink : public SinkPlugin {
public:
    const char* Name() const override { return "stream_sink"; }

    Status Write(std::shared_ptr<const DataBatch> batch) override {
        auto frame = Serialize(batch);  // MessagePack
        ws_manager_->Broadcast(feature_name_, frame);
        return Status::Ok();
    }

private:
    WebSocketManager* ws_manager_;
    std::string feature_name_;
};
```

**零拷贝关键**: `DataBatch` 用 `shared_ptr<const>` 传递，StreamSink 和 RecordingSink 共享同一份数据，不复制。

### 6.3 RecordingSink — 高性能磁盘写入

```cpp
class RecordingSink : public SinkPlugin {
public:
    Status Init(const RecordingParams& params) {
        path_ = params.dir + "/" + params.feature + "_" +
                FormatTimestamp(Now()) + ".ilr";
        file_ = std::fopen(path_.c_str(), "wb");
        WriteFileHeader(params);
        return Status::Ok();
    }

    Status Write(std::shared_ptr<const DataBatch> batch) override {
        if (bytes_written_ >= max_bytes_) {
            return Status::Error("size limit");
        }

        auto payload = SerializeBatch(batch);

        RecordHeader hdr{batch->timestamp_ns(), (uint32_t)payload.size()};
        fwrite_unlocked(&hdr, sizeof(hdr), 1, file_);
        fwrite_unlocked(payload.data(), payload.size(), 1, file_);

        bytes_written_ += sizeof(hdr) + payload.size();
        record_count_++;

        if (record_count_ % 1000 == 0) {
            index_entries_.push_back({hdr.timestamp_ns, ftell(file_)});
        }

        return Status::Ok();
    }

    Status Finalize() {
        WriteIndexBlock();
        WriteFooter();
        fclose(file_);
        return Status::Ok();
    }

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint64_t max_bytes_;
    std::atomic<uint64_t> bytes_written_{0};
    uint64_t record_count_ = 0;
    std::vector<IndexEntry> index_entries_;
};
```

**性能优化**:
- 使用 `fwrite_unlocked`（单线程写入，无需锁）
- 不做 `fsync`（交给 OS page cache，停止录制时才 flush）
- 批量索引（每 1000 条一个索引点，非每条）

### 6.4 ReplayEngine — 时间驱动的数据回放

```cpp
class ReplayEngine {
public:
    struct Session {
        std::string id;
        std::string file_path;
        std::string feature_name;
        uint64_t total_records;
        uint64_t duration_ns;
        std::vector<IndexEntry> index;

        enum State { kLoaded, kPlaying, kPaused, kFinished };
        State state = kLoaded;
        double speed = 1.0;
        uint64_t current_pos = 0;
        uint64_t current_ts = 0;
    };

    StatusOr<std::string> Load(const std::string& path);
    Status Play(const std::string& session_id, double speed = 1.0);
    Status Pause(const std::string& session_id);
    Status Seek(const std::string& session_id, uint64_t timestamp_ns);

private:
    void ReplayLoop(Session& session);
};
```

---

## 七、API 设计（精简）

### 7.1 Feature 控制

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/features` | 列出所有 Feature 及状态 |
| POST | `/api/v1/features/:name/start` | 启动 Feature |
| POST | `/api/v1/features/:name/stop` | 停止 Feature |
| POST | `/api/v1/features/:name/pause` | 暂停 Feature |
| POST | `/api/v1/features/:name/resume` | 恢复 Feature |

### 7.2 录制

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/features/:name/record/start` | 开始录制 |
| POST | `/api/v1/features/:name/record/stop` | 停止录制 |
| GET | `/api/v1/features/:name/record/status` | 录制进度 |
| GET | `/api/v1/recordings` | 列出所有录制文件 |
| DELETE | `/api/v1/recordings/:id` | 删除录制 |
| GET | `/api/v1/recordings/:id/download` | 下载录制文件 |

### 7.3 回放

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/replay/load` | 加载录制文件（上传或引用） |
| WS | `/ws/replay/:session_id` | 回放数据流 + 控制命令 |

### 7.4 WebSocket 端点

| 端点 | 协议 | 说明 |
|------|------|------|
| `/ws/stream/:feature` | Binary (MsgPack) | 实时数据流 |
| `/ws/replay/:session` | Binary (MsgPack) | 回放数据流 |
| `/ws/control` | JSON | 全局状态变更事件推送 |

---

## 八、前端架构

### 8.1 组件层次

```
App
+-- Layout
|   +-- Sidebar (Feature 列表 + 状态指示灯)
|   +-- StatusBar (活跃 Feature 数 + 录制指示)
|   +-- Content
|       +-- FeaturePanel (通用壳)
|           +-- ControlBar (启动/暂停/录制按钮)
|           +-- DataView (具体可视化，按 feature.frontend.type 渲染)
|           |   +-- TimeSeriesChart (cpu_utilization, process_cpu)
|           |   +-- FlameGraphView (offcpu, cpu_profiler)
|           |   +-- TimelineView (sched_analysis)
|           |   +-- TableView (io_monitor, net_tracer)
|           +-- RecordingOverlay (录制中的状态覆盖层)
|
+-- RecordingManager (全局录制状态管理)
+-- ReplayModal (回放弹窗 + 控制条)
+-- SettingsPage (Feature 配置编辑)
```

### 8.2 FeaturePanel 通用组件

```tsx
function FeaturePanel({ feature }: { feature: FeatureConfig }) {
  const { state, buffer, start, stop, pause, resume } = useFeatureStream(feature.name);
  const { isRecording, startRecord, stopRecord, progress } = useRecording(feature.name);

  return (
    <div className="feature-panel">
      <ControlBar
        state={state}
        isRecording={isRecording}
        onStart={start}
        onStop={stop}
        onPause={pause}
        onResume={resume}
        onRecord={startRecord}
        onStopRecord={stopRecord}
      />

      {state === 'inactive' ? (
        <EmptyState
          icon={feature.icon}
          message={`${feature.display_name} 未启动`}
          action="点击上方按钮开始采集"
        />
      ) : (
        <DataView
          type={feature.frontend.type}
          data={buffer}
          frozen={state === 'paused'}
        />
      )}

      {isRecording && (
        <RecordingIndicator progress={progress} />
      )}
    </div>
  );
}
```

### 8.3 EmptyState 设计

```
┌───────────────────────────────────────────────────────┐
│                                                       │
│              ┌──────────────────┐                     │
│              │   (icon: cpu)    │                     │
│              │                  │                     │
│              │     ░░░░░░░░     │                     │
│              │   ░░░░░░░░░░    │                     │
│              └──────────────────┘                     │
│                                                       │
│            CPU Utilization 未启动                      │
│                                                       │
│         点击工具栏中的启动按钮开始采集                   │
│                                                       │
│              [ 启动此功能 ]                            │
│                                                       │
└───────────────────────────────────────────────────────┘
```

---

## 九、Pipeline 动态 Sink 热附着

### 9.1 Fanout 机制

Pipeline 的 Sink 阶段改为 Fanout 模式，支持动态附着/分离：

```cpp
class SinkFanout {
public:
    void AddSink(const std::string& id, std::shared_ptr<SinkPlugin> sink) {
        std::unique_lock lock(mutex_);
        sinks_[id] = std::move(sink);
    }

    void RemoveSink(const std::string& id) {
        std::unique_lock lock(mutex_);
        sinks_.erase(id);
    }

    void Deliver(std::shared_ptr<const DataBatch> batch) {
        std::shared_lock lock(mutex_);
        for (auto& [id, sink] : sinks_) {
            sink->Write(batch);
        }
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SinkPlugin>> sinks_;
};
```

**线程安全**:
- `Deliver()` 持有读锁（共享），高频操作，多个并发 Deliver 不互斥
- `AddSink/RemoveSink` 持有写锁（独占），极低频操作
- 典型场景：Deliver 每秒数百次，Add/Remove 在整个运行期间只发生几次

### 9.2 启动时自动注入 StreamSink

```cpp
Status FeatureManager::Start(const std::string& feature_name) {
    auto& f = features_[feature_name];

    // 1. 创建 Pipeline
    auto pipeline = CreatePipeline(f.pipeline_config);

    // 2. 自动注入 StreamSink（所有 Feature 都有实时推送）
    auto stream_sink = std::make_shared<StreamSink>(ws_manager_, feature_name);
    pipeline->sink_fanout().AddSink("__stream__", stream_sink);

    // 3. 启动
    pipeline->Start();
    f.pipeline = std::move(pipeline);
    f.state = FeatureState::kActive;

    // 4. 通知前端
    NotifyStateChange(feature_name, "active");
    return Status::Ok();
}
```

### 9.3 录制时动态附着 RecordingSink

```cpp
StatusOr<std::string> FeatureManager::StartRecording(
    const std::string& feature_name, const RecordingParams& params) {

    auto& f = features_[feature_name];
    if (f.state != FeatureState::kActive) {
        return Status::Error("feature must be active to record");
    }

    auto recording_sink = std::make_shared<RecordingSink>();
    recording_sink->Init(params);

    f.pipeline->sink_fanout().AddSink("__recording__", recording_sink);
    f.is_recording = true;
    f.recording_sink = recording_sink;

    return recording_sink->GetRecordingId();
}
```

---

## 十、性能分析

### 10.1 热路径开销

| 操作 | 频率 | 开销 | 瓶颈分析 |
|------|------|------|---------|
| Source -> Fanout | 1-1000 Hz | shared_ptr copy (~20ns) | 无瓶颈 |
| Fanout -> StreamSink | 同上 | serialize + WS send (~50-500us) | WS 写缓冲 |
| Fanout -> RecordingSink | 同上 | serialize + fwrite (~10-100us) | 磁盘 I/O (OS buffer) |
| 前端接收 + Ring Buffer | 同上 | deserialize + push + evict (~100us) | 可忽略 |
| 前端渲染 | 60 FPS | DOM diff (~2-16ms) | 正常 React 开销 |

### 10.2 录制对实时性能的影响

```
无录制:   Source -> Process -> Fanout -> StreamSink -> WS
                                                      (~50us)

有录制:   Source -> Process -> Fanout --+--> StreamSink -> WS (不变)
                                        |
                                        +--> RecordingSink -> Disk (SinkPool 异步)
```

**影响接近零**：
1. Fanout 是 `shared_ptr<const DataBatch>` 的引用计数增加，不是数据拷贝
2. RecordingSink 在 SinkPool 线程中执行，与 StreamSink 并行
3. 即使磁盘写入慢，也不阻塞实时推送

### 10.3 内存使用

| 组件 | 内存占用 | 说明 |
|------|---------|------|
| Inactive Feature | ~0 | 无 pipeline 实例 |
| Active Feature (CPU util) | ~200KB | Pipeline + channel + small batches |
| Active Feature (eBPF) | ~2-10MB | BPF maps + stack storage |
| 前端 Ring Buffer (60s@1Hz) | ~50KB | 60 条记录 |
| 前端 Ring Buffer (10s@1KHz) | ~5MB | 10K 条事件 |
| RecordingSink buffer | ~64KB | fwrite OS buffer |

### 10.4 与当前架构的性能对比

| 指标 | 当前 (全部启动) | 新方案 (按需启动) |
|------|----------------|-----------------|
| 启动内存 | ~50MB (所有 pipeline) | ~10MB (仅基础服务) |
| eBPF 内核开销 | 始终存在 | 仅用户启用时存在 |
| CPU 基线 | ~3% (定时采集) | ~0.5% (无活跃 Feature) |
| 首次数据延迟 | 0 (已在采集) | ~100-500ms (启动延迟) |
| 磁盘写入 | 持续 (local_storage) | 仅录制时写入 |

---

## 十一、架构 Review

### 11.1 设计原则检查

| 原则 | 评估 | 说明 |
|------|------|------|
| **单一职责** | PASS | FeatureManager 管生命周期，Pipeline 管数据流，Sink 管输出 |
| **零成本抽象** | PASS | Inactive Feature 不分配任何资源 |
| **关注点分离** | PASS | 控制面(API) 与数据面(Pipeline) 完全分离 |
| **无冗余** | PASS | 去掉了 ConfigStore 三层合并、独立 LifecycleManager、7 态 FSM |
| **可测试性** | PASS | DataSource 接口使前端可 mock，RecordingSink 可独立测试 |
| **渐进增强** | PASS | Phase 1 只加 start/stop + WS stream，不改核心 Pipeline |

### 11.2 潜在问题与缓解

| 问题 | 风险等级 | 缓解方案 |
|------|---------|---------|
| eBPF 启动延迟 (BPF 加载 + attach) | 中 | Pause 不 detach，Resume 瞬间恢复 |
| WebSocket 断开时丢数据 | 低 | 前端自动重连 + 后端保留最近 N 条供回填 |
| 录制文件损坏（断电） | 中 | Footer 丢失时顺序扫描恢复 Records |
| 多 Feature 同时 Active | 中 | 限制同时活跃数（默认 5） |
| Fanout shared_mutex 竞争 | 极低 | 读多写极少，shared_lock 无竞争 |

### 11.3 对标开源方案的简化

| 开源方案特性 | 是否采用 | 理由 |
|-------------|---------|------|
| Netdata DynCfg 完整 tree | 否 | 组件数少，不需要层级管理 |
| Vector topology diff | 否 | 1:1 映射，不存在拓扑变更 |
| Alloy DAG reconciliation | 否 | 无组件间依赖关系 |
| OTel 7-state FSM | 否 | 简化为 3+2 状态 |
| OTel confmap provider | 否 | 单文件 YAML 足够 |
| Vector tap (采样调试) | 是(变体) | 录制模式 = tap 的持久化版本 |

---

## 十二、eBPF 插件的 Pause/Resume 语义

eBPF 插件的 Pause/Resume 需要特殊处理（避免昂贵的 re-attach）：

| 操作 | CPU Profiler | Off-CPU Profiler | Sched Tracer |
|------|-------------|-----------------|--------------|
| **Start** | 加载 BPF obj + attach perf_event | 加载 + attach tracepoint | 加载 + attach |
| **Pause** | 停止读取 perf buffer | 停止读取 ring buffer | 停止消费 |
| **Resume** | 恢复读取 | 恢复读取 | 恢复消费 |
| **Stop** | detach + 释放 maps + 关闭 fd | detach + 释放 | detach + 释放 |

**关键**: Pause 时 BPF 程序仍然 attached 在内核中，但用户态不消费事件。
- perf buffer / ring buffer 满了会自动覆盖旧数据
- 不会导致内核内存无限增长
- Resume 后立即开始收到新数据（Pause 期间的数据被丢弃，这是预期行为）

---

## 十三、实施路线图

### Phase 1: 按需启停 + 实时流 (1 周)

```
后端:
  - FeatureManager 类 (生命周期管理)
  - StreamSink (替代 websocket_sink)
  - SinkFanout (替代固定 sink 列表)
  - API: /features/start, /stop, /pause, /resume
  - WebSocket: /ws/stream/:feature

前端:
  - FeaturePanel 通用组件
  - ControlBar (启动/暂停按钮)
  - EmptyState (未启动状态)
  - useFeatureStream hook
  - TimeSeriesBuffer

预计: ~500 行后端 + ~400 行前端
```

### Phase 2: 录制 (1-2 周)

```
后端:
  - RecordingSink (.ilr 格式写入)
  - RecordingStore (文件索引管理)
  - API: /record/start, /stop, /status
  - 磁盘配额管理

前端:
  - RecordingControl 组件
  - RecordingIndicator (录制状态覆盖层)
  - RecordingsPage (历史录制列表)

预计: ~400 行后端 + ~300 行前端
```

### Phase 3: 回放 (1-2 周)

```
后端:
  - ReplayEngine (文件解析 + 时间驱动推送)
  - 文件上传 API
  - WebSocket: /ws/replay/:session

前端:
  - ReplayDataSource
  - ReplayControlBar (play/pause/seek/speed)
  - ReplayModal (文件选择 + 上传)

预计: ~400 行后端 + ~300 行前端
```

### Phase 4: 优化 (1 周)

```
- MessagePack 替代 JSON
- 录制文件 zstd 流式压缩
- 多 Feature 关联回放（时间对齐）
- idle_timeout 自动停止
- 自动清理策略执行
```

---

## 十四、FAQ

**Q: 用户关闭浏览器后 Feature 会停止吗？**

不会。Feature 生命周期由后端管理。重新打开浏览器时，前端查询 `/api/v1/features` 获取当前状态，看到仍为 Active 并重新建立 WS 订阅。可选启用 `idle_timeout` 实现无人值守自动停止。

**Q: Pause 和 Stop 的区别是什么？**

Pause 保留 Pipeline 实例和 BPF 附着（恢复 ~0ms）。Stop 销毁一切（重新启动需 ~100-500ms BPF 加载）。Pause 适合临时离开，Stop 适合确认不再需要。

**Q: 录制文件能跨机器使用吗？**

能。.ilr 是自描述格式，不依赖本机环境。A 机器录制，B 机器上传回放。

**Q: 能否同时录制多个 Feature？**

能。每个 Feature 独立录制到各自 .ilr 文件。关联分析通过时间戳对齐实现。

**Q: 为什么不用 SQLite 做录制？**

SQLite 适合查询，但高频追加写入性能差（WAL 开销）。.ilr 顺序追加吞吐量是 SQLite 的 10-50x。需要复杂查询时可后转为 SQLite。

**Q: 多人同时看同一个 Feature？**

StreamSink 做广播：一份序列化后发给所有 WS 连接。增加观察者接近零成本。

**Q: 在 Docker 容器中运行时 PID 不匹配怎么办？**

已解决。eBPF 的 `bpf_get_current_pid_tgid()` 返回宿主机命名空间 PID，而容器内 `/proc` 显示容器 PID。前端 ProfileSnapshot 组件通过以下策略自动适配：

1. 从 Thread Breakdown 获取目标进程的已知线程名列表
2. 在 Profile 数据中查找与已知线程名匹配的样本
3. 提取该样本的 BPF PID（即宿主机 PID）
4. 用宿主机 PID 过滤所有属于目标进程的样本

此方案无需修改 eBPF 程序或内核配置，纯前端逻辑自适应。

---

## 十五、容器化部署注意事项

### PID 命名空间

| 场景 | 行为 | 影响 |
|------|------|------|
| `--pid=host` | 容器与宿主共享 PID 空间 | 无 PID 差异 |
| 默认 PID 隔离 | eBPF 报告宿主 PID | 前端自动通过线程名匹配 |
| 嵌套容器 | 多层 PID 映射 | 同上，通过线程名匹配 |

### 特权要求

- `--privileged` 或 `--cap-add=SYS_ADMIN,SYS_PTRACE`：eBPF 程序加载
- `--cap-add=PERFMON`（kernel 5.8+）：perf_event 采样
- `/proc` 和 `/sys/kernel/debug` 挂载：符号解析和 BPF 文件系统

---

## 十六、Phase 2 实现：分层激活 + WebSocket + 资源预算

### 16.1 Feature Tier 系统

每个 Feature 注册时分配一个 Tier：

```cpp
enum class FeatureTier {
    kMonitoring = 1,  // procfs 读取，< 0.5% CPU，自动启动
    kTracing = 2,     // 轻量 eBPF，1-3% CPU，自动启动
    kProfiling = 3,   // 高频采样，3-10% CPU，用户手动触发
};
```

前端通过 `usePageActivation(category, features)` hook 实现自动启动：
- 进入标签页 → 自动 `POST /features/:name/start` 所有 Tier 1/2
- Tier 3 保留为显式操作按钮

### 16.2 WebSocket 与 Feature 系统的集成

FeatureManager 在启动 Pipeline 时自动注入 `WebSocketSink`：

```
Feature Start → Pipeline Build → [StreamSink + RecordingSink + WebSocketSink]
                                         ↓
                              WebSocketSinkStore (key = feature_name)
                                         ↓
                              WebSocketManager 广播循环
                                         ↓
                              前端 WS 客户端实时接收
```

前端 `LiveDataSource` 自动降级策略：
1. 尝试 WebSocket 连接 → 成功则零延迟推送
2. WS 断开 → 切换 HTTP 轮询 (1s 间隔)
3. WS 重连成功 → 自动恢复推送

### 16.3 资源预算 API

```
GET /api/v1/budget → {
  "usage": { "rss_bytes", "cpu_pct", "active_features", "ebpf_probes" },
  "limits": { "max_memory_bytes", "max_cpu_pct", "max_ebpf_probes" },
  "exceeded": false
}
```

前端侧边栏底部实时显示资源指示器，颜色编码：
- 绿色: < 50% 预算
- 黄色: 50-80%
- 红色: > 80%
