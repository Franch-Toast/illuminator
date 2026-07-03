# Illuminator 前端架构设计文档

> **版本**: 3.0  
> **日期**: 2026-07-02  
> **状态**: 已确认（RFC v3 架构）

---

## 一、核心架构决策

### 1.1 双模式架构：Live + Replay

前端分为两个顶级功能模式：

```
┌─────────────────────────────────────────────────────────────────────┐
│  ILLUMINATOR  v0.3.0                                                │
│  [📡 Live]  [📂 Replay]                           [⏺ 录制中 00:32] │
├─────────────────────────────────────────────────────────────────────┤
│  Overview │ CPU │ Memory │ IO │ Network │ GPU │ System              │
└─────────────────────────────────────────────────────────────────────┘
```

| 模式 | 用途 | 数据来源 | 后端行为 |
|------|------|---------|---------|
| **Live** | 实时监控 + 性能分析 + 录制 | 后端 Pipeline 实时采集 | Pipeline 运行中 |
| **Replay** | 上传录制文件，回放分析 | 本地 .ilr 文件 | 无 Pipeline，纯解析 |

**设计理由**：
- Live 和 Replay 的交互模型完全不同（实时流 vs 时间轴回放）
- 分离后各自的 UI 逻辑更清晰，不需要 if/else 判断当前是实时还是回放
- Replay 模式无后端资源开销，可离线使用

### 1.2 导航结构：按功能类别分标签页

```
功能标签页: Overview │ CPU │ Memory │ IO │ Network │ GPU │ System
```

两种模式共享相同的标签页结构，区别仅在于数据来源和交互方式。

---

## 二、Live 模式设计

### 2.1 分层激活策略（替代逐面板启停）

**核心理念**：用户不应该手动管理每个小面板的启停。系统根据功能的开销等级自动决定何时激活。

将所有 Feature 按系统开销分为三个层级：

| 层级 | 特征 | 开销 | 激活方式 | 典型示例 |
|------|------|------|---------|---------|
| **Tier 1 — 监控** | 读取 procfs 文件 | < 0.5% CPU | **自动**：进入标签页即激活 | `cpu_utilization`, `process_cpu` |
| **Tier 2 — 追踪** | 轻量 eBPF + procfs | 1-3% CPU | **自动**：进入标签页即激活 | `sched_analysis`, `io_monitor`, `net_tracer` |
| **Tier 3 — 剖析** | 高频 eBPF 采样 | 3-10% CPU | **手动**：用户显式触发 | `cpu_profile`, `offcpu_profile`, `heap_profiler` |

**行为规则**：

1. **进入标签页 → Tier 1/2 自动启动**
   - 用户打开 CPU 页，`cpu_utilization` 和 `process_cpu` 立即开始采集
   - 图表直接显示实时数据，无需任何点击操作
   - 体验等同于打开 htop 或 Grafana Dashboard

2. **离开标签页 → 可选自动停止**
   - 配置项：`auto_stop_on_leave: true/false`（默认 false）
   - false = 后台继续运行（数据持续累积，适合服务器场景）
   - true = 离开即停止（节省资源，适合嵌入式/车端场景）

3. **Tier 3 重量级操作 → 显式动作按钮**
   - 不是一个"始终存在但空着的面板"
   - 而是一个**操作按钮**："开始 CPU Profile" → 运行 → 生成结果
   - 持续模式：Profile 持续运行直到用户手动停止
   - 结果展示：火焰图在 Profile 运行期间持续累积构建

### 2.2 资源预算系统

后端维护一个全局资源预算，防止 Illuminator 自身影响被观测系统：

```
┌─ 资源预算指示器 (右上角) ────────────────────────────────┐
│  System Overhead: ████░░░░░░ 38% / 100%                   │
│  CPU: 2.1% | Memory: 45MB | eBPF probes: 3/8             │
└───────────────────────────────────────────────────────────┘
```

| 预算维度 | 默认上限 | 可配置 |
|---------|---------|--------|
| CPU 开销 | ≤ 5% | ✓ |
| 内存占用 | ≤ 200MB | ✓ |
| eBPF 探针数 | ≤ 8 | ✓ |
| 磁盘写入（录制） | ≤ 50MB/min | ✓ |

当用户尝试启动 Tier 3 操作时，如果超出预算，前端提示：
```
"CPU Profile 需要约 3% CPU 开销。当前预算剩余 2%。
 [继续启动（超出预算）] [取消] [停止其他 Feature 释放资源]"
```

### 2.3 页面交互模式

#### Live — CPU 页面示例

```
┌───────────────────────────────────────────────────────────────────┐
│  CPU  [System] [Process]                                          │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─ CPU Utilization ──────────────────────────────────────────┐   │
│  │  ████████████████████████████████████████████████████████  │   │
│  │  (堆叠面积图 - 自动运行，无需用户操作)                      │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌─ Per-Core ─────────────────────────────────────────────────┐   │
│  │  (热力图 - 自动运行)                                        │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌ 摘要卡片 ┐ ┌ 摘要卡片 ┐ ┌ 摘要卡片 ┐ ┌ 摘要卡片 ┐            │
│  │ Avg CPU  │ │ Hot Core │ │ CtxSwitch│ │ RunQueue │            │
│  │ 34.2%    │ │ Core #7  │ │ 12.4k/s  │ │ 2.1      │            │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘            │
│                                                                   │
│  没有 [启动] 按钮 — 一切自动运行                                  │
└───────────────────────────────────────────────────────────────────┘
```

#### Live — Process Detail 示例

```
┌───────────────────────────────────────────────────────────────────┐
│  ← Back │ nginx (PID:1234)                                        │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─ CPU Timeline ─────────────────────────────────────────────┐   │
│  │  (自动运行 - Tier 1)                                        │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌─ Thread Breakdown ─────────────────────────────────────────┐   │
│  │  (自动运行 - Tier 1)                                        │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌─ CPU Profiling ────────────────────────────────────────────┐   │
│  │                                                            │   │
│  │  对该进程进行 CPU 采样分析，生成火焰图。                     │   │
│  │  采样频率: 49Hz | 预计开销: ~3% CPU                         │   │
│  │                                                            │   │
│  │  [▶ 开始 On-CPU Profile]  [▶ 开始 Off-CPU Profile]         │   │
│  │                                                            │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ↓ 用户点击 "开始 On-CPU Profile" 后:                              │
│                                                                   │
│  ┌─ On-CPU Profile (运行中 · 已采集 847 样本) ───── [⏹ 停止] ─┐   │
│  │  ████████████████████████████████████████████████████████  │   │
│  │  █ worker::run  █ Pipeline::Process █ epoll_wait         █ │   │
│  │  █ handle_req   █ Symbolize         █                    █ │   │
│  │                                                            │   │
│  │  Top Functions:                                            │   │
│  │  1. epoll_wait        22.3%                               │   │
│  │  2. Pipeline::Process 18.7%                               │   │
│  │  3. worker::run       12.1%                               │   │
│  └────────────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────────┘
```

### 2.4 全局录制

录制不是每个面板独立控制，而是**全局会话录制**：

```
点击右上角 [⏺ 录制] → 所有当前活跃的 Feature 同时开始写入磁盘
    ↓
[⏺ 录制中 02:15 | 12.3MB]    ← 实时显示时长和文件大小
    ↓
点击 [⏹ 停止录制] → 生成 .ilr 文件，可在 Replay 模式中回放
```

**录制策略**：
- 录制捕获所有当前 Active 的 Feature 数据
- 单个 .ilr 文件包含多个 Feature 的时间对齐数据
- 硬限制：单次录制最大 500MB（可配置），超出自动停止
- 录制不改变任何 Feature 的运行状态（纯旁路写入）

**实现方式（RFC v3）**：
- **后端录制**：`useRecording()` hook 调用 REST API 控制后端 RecordingSink 落盘
  - `POST /api/v1/features/:name/record/start` — 开始写入 .ilr 文件
  - `POST /api/v1/features/:name/record/stop` — 停止录制
- **前端保存**：`useSaveBuffer()` hook 将 DataBus 内存 ringBuffer 序列化为 .ilr 文件并下载（无需后端 I/O）

---

## 三、Replay 模式设计

### 3.1 架构核心：DataSource 抽象

Replay 和 Live 共享完全相同的图表组件和数据处理 hooks，区别仅在于数据来源。
通过 `DataSource` 接口抽象实现零冗余：

```
┌─ 前端架构 ─────────────────────────────────────────────────────────┐
│                                                                    │
│  ┌─ 图表组件 + 数据 hooks（100% 共享）─────────────────────────┐   │
│  │  CpuPage / MemoryPage / ... → ECharts 渲染                  │   │
│  └──────────────────────────────────────┬──────────────────────┘   │
│                                         │                          │
│                                    DataSource 接口                  │
│                                    subscribe(feature, cb)          │
│                                    getLatest(feature)              │
│                                         │                          │
│                    ┌────────────────────┼────────────────────┐     │
│                    │                                         │     │
│            ┌───────┴────────┐                     ┌─────────┴───┐ │
│            │    DataBus     │                     │ReplaySource │ │
│            │ (SSE via       │                     │ (文件解析)   │ │
│            │  SseLink)      │                     │             │ │
│            └────────────────┘                     └─────────────┘ │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

```typescript
// 统一接口 — 图表组件不关心数据来自哪里
interface DataSource {
  subscribe(feature: string, cb: (batch: DataBatch) => void): () => void
  getLatest(feature: string): DataBatch | null
  getAvailableFeatures(): string[]
}
```

**不冗余的关键**：
- 图表组件只依赖 `DataSource` 接口，不直接调用 API 或读取文件
- Live 切换到 Replay 只是替换 DataSource 实例，UI 组件零修改
- 新增标签页（Memory/IO/Network）自动支持 Replay，无额外代码

### 3.2 .ilr 文件格式

延用后端 `RecordingSink` 已实现的 NDJSON 格式（每行一个 JSON 对象）：

```
{"type":"header","version":1,"features":["cpu_utilization","process_cpu"],"start_ts":1718150400000,"end_ts":1718150712000}
{"ts":1718150400000,"feature":"cpu_utilization","data":{"user_pct":12.3,"system_pct":5.1,...}}
{"ts":1718150400000,"feature":"process_cpu","data":{"processes":[...]}}
{"ts":1718150401000,"feature":"cpu_utilization","data":{"user_pct":13.1,...}}
...
```

**格式优势**：
- 人类可读（可用 `jq` 调试）
- 流式可解析（不需要加载整个文件到内存）
- 与后端 RecordingSink 输出格式一致，零转换
- gzip 压缩后体积缩小 5-10x

### 3.3 ReplayEngine（前端纯客户端）

```typescript
class ReplayEngine implements DataSource {
  private frames: IndexedFrame[]     // 按时间排序的帧索引
  private currentTime: number        // 当前播放时间戳
  private speed: number = 1          // 播放速度
  private subscribers: Map<string, Set<Callback>>

  async loadFile(file: File): Promise<ReplayMeta> {
    // 1. 流式解析 NDJSON（ReadableStream + TextDecoder）
    // 2. 构建时间索引（只存 ts + offset，不缓存全部 data）
    // 3. 返回元数据供 UI 展示
  }

  play() {
    // requestAnimationFrame 驱动：
    // 每帧推进 currentTime，查找该时间点的数据帧，通知 subscribers
  }

  seek(timestamp: number) {
    // 二分查找跳转到目标时间，立即发射当前帧
  }
}
```

**设计决策：前端纯客户端解析，不依赖后端**

| 方案 | 优点 | 缺点 |
|------|------|------|
| ~~后端解析，API 返回~~ | 大文件友好 | 多一层网络、后端复杂度增加、不可离线 |
| **前端直接解析** | 零后端改动、可离线、响应快 | 超大文件需要分段加载 |

对于超大文件（>100MB），采用 **分段流式解析**：只构建时间索引，按需读取 data 字段。

### 3.4 UI 设计

**入口页**：

```
┌───────────────────────────────────────────────────────────────────┐
│  [📂 Replay 模式]                                                  │
│                                                                   │
│  拖拽 .ilr 文件到此处，或 [选择文件]                                │
│                                                                   │
│  最近加载:                                                         │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │  session_2026-06-12_14-30.ilr                                │  │
│  │  时长: 5m12s  |  大小: 23.4MB  |  Features: cpu_util, ...   │  │
│  │  [▶ 回放]                                                    │  │
│  └─────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
```

**回放页面（与 Live 完全相同的标签页，底部加时间轴）**：

```
┌───────────────────────────────────────────────────────────────────┐
│  CPU  [System] [Process]               📂 session_2026-06-12.ilr   │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  (同 Live 模式的图表，数据来自 ReplayEngine)                       │
│                                                                   │
├───────────────────────────────────────────────────────────────────┤
│  [◀◀] [▶] [▶▶]  1x▼   00:01:23 / 05:12                          │
│  ════════════════════●═══════════════════════════════════════════  │
└───────────────────────────────────────────────────────────────────┘
```

### 3.5 Replay 不引入后端任何改动

| 组件 | 改动 |
|------|------|
| 后端 RecordingSink | 无（已输出 .ilr 格式） |
| 后端 API | 无 |
| 前端图表组件 | 无（通过 DataSource 接口消费数据） |
| 前端新增 | `ReplayEngine` + `ReplayControlBar` + 文件加载 UI |

总新增代码量预估：~400 行 TypeScript。

---

## 四、标签页设计

### 4.1 通用标签页结构

```
功能标签页
├── [System 子标签]  系统级图表（Tier 1/2 自动运行）
└── [Process 子标签]  进程级
    ├── Top-N 进程列表（Tier 1 自动运行）
    └── Process Detail:
        ├── Timeline + Threads（Tier 1 自动运行）
        └── Profiling 操作区（Tier 3 显式触发）
```

### 4.2 各标签页

| 标签页 | System 子标签 | Process 子标签 | Tier 3 操作 |
|--------|-------------|---------------|------------|
| **CPU** | 利用率面积图 + 核心热力图 + 摘要 | 进程 CPU 排名 → Detail | On-CPU/Off-CPU Profile |
| **Memory** | Used/Cached/Free 面积图 + Swap | 进程 RSS 排名 → Detail | 堆分析 (jemalloc/tcmalloc) |
| **IO** | IOPS + 吞吐 + 延迟直方图 | 进程 IO 排名 → Detail | IO 追踪 (bio latency) |
| **Network** | 接口流量 + 连接数 + 重传 | 进程流量排名 → Detail | TCP 追踪 |
| **GPU** | 利用率 + 显存 + 温度 | 进程 GPU 排名 → Detail | GPU Profile |
| **System** | 调度延迟 + 中断 + 运行队列 | — | 详细调度追踪 |

### 4.3 Feature 与标签页映射

| Feature | Category | Tier | 激活方式 |
|---------|----------|------|---------|
| `cpu_utilization` | cpu | 1 | 进入 CPU 页自动启动 |
| `process_cpu` | cpu | 1 | 进入 CPU/Process 页自动启动 |
| `cpu_profile` | cpu | 3 | 用户在 Process Detail 中点击启动 |
| `offcpu_profile` | cpu | 3 | 用户在 Process Detail 中点击启动 |
| `memory_utilization` | memory | 1 | 进入 Memory 页自动启动（前端 placeholder，后端 FeatureDriver 待实现） |
| `memory_processes` | memory | 1 | 进入 Memory/Process 页自动启动（前端 placeholder，后端 FeatureDriver 待实现） |
| `heap_profiler` | memory | 3 | 用户在 Process Detail 中点击启动 |
| `io_monitor` | io | 2 | 进入 IO 页自动启动 |
| `net_tracer` | network | 2 | 进入 Network 页自动启动 |
| `sched_analysis` | scheduler | 2 | 进入 System 页自动启动 |
| `gpu_monitor` | gpu | 1 | 进入 GPU 页自动启动（前端 placeholder，后端 FeatureDriver 待实现） |

---

## 五、图表技术选型

### 5.1 现状问题

当前使用纯手写 SVG，缺少：Tooltip、Zoom/Pan、坐标轴格式化、图例交互、响应式适配。

### 5.2 推荐方案：Apache ECharts

| 决策因素 | 分析 |
|---------|------|
| 性能 | Canvas 渲染，100k+ 数据点流畅；实际场景 60-2000 点 |
| 图表种类 | 面积图、热力图、折线图、直方图、饼图原生支持 |
| 实时流 | `setOption` 增量更新，适配 SSE 推送 |
| 交互 | Tooltip、DataZoom、Brush、Legend 开箱即用 |
| 暗色主题 | 内置 dark 主题 |
| Bundle | 按需引入 ~80KB gzip |

**Benchmark 参考（2026）**：

| 库 | Bundle (gzip) | 10k 点 | 100k 点 | 图表种类 |
|---|---|---|---|---|
| **Apache ECharts** | ~80KB | 流畅 | 流畅 | 极丰富 |
| uPlot | ~48KB | 流畅 | 流畅 | 仅时序 |
| Recharts | ~50KB | 卡顿 | 不可用 | 中等 |

**不选 uPlot**：性能极致但仅支持时序线图，无热力图/直方图。  
**不选 Recharts**：SVG 渲染，1000+ 点时卡顿；无热力图。  
**保留自定义 SVG**：火焰图 + 表格内 Sparkline（ECharts 过重的微型场景）。

### 5.3 ECharts 使用规范

```typescript
// 按需引入
import * as echarts from 'echarts/core'
import { LineChart, HeatmapChart, BarChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, DataZoomComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'

echarts.use([LineChart, HeatmapChart, BarChart,
             GridComponent, TooltipComponent, DataZoomComponent, CanvasRenderer])
```

---

## 六、数据流（统一架构）

### 6.1 DataSource 统一模型

图表组件通过数据 hooks 消费数据，不感知来源。Live 模式下 hooks 通过 `getDataSource()` 获取全局 `dataBus` 单例并订阅 Feature：

```typescript
// 页面组件中的使用方式（Live/Replay 完全相同）
function SystemSubTab() {
  const { areaData } = useCpuUtilization(true)  // 内部 getDataSource().subscribe(...)
  return <EChartsAreaChart option={buildOption(areaData)} />
}
```

Feature 列表由 `useFeatureList()` hook 获取，调用 `GET /api/v2/features` 返回 `{"features": [...descriptors]}`。

### 6.2 Live 模式实现

```
App 启动 → dataBus.connect()
    ↓
POST /api/v1/events/subscribe  { features: [...] }
    ↓ 返回 { subscription_id, url }
GET /api/v1/events/{id}  (SSE 长连接，SseLink 封装 EventSource)
    ↓
SSE event "data" → DataBus.handleData(feature, payload)
SSE event "frame" → DataBus.handleFrame()  (大 payload 分片重组)
    ↓
hook: getDataSource().subscribe('cpu_utilization', cb)
    ↓
extractRecords(batch.data) → 解析 metrics/records 数组
    ↓
callback → hook 更新 state → ECharts 重绘
```

动态增删 Feature 订阅：`POST /api/v1/events/{id}/update`（DataBus.syncSubscription）。

**SSE payload 格式**：
- `time_series` 模型：`metrics` 数组，元素为 `{ labels, fields, timestamp }`
- `trace` / `generic` 模型：`records` 数组
- 前端 `extractRecords()`（`web/src/utils/ssePayload.ts`）统一处理两种格式

### 6.3 Replay 模式实现

```
用户上传 .ilr 文件
    ↓
ReplayEngine.loadFile() → 构建时间索引
    ↓
ReplayEngine.play()
    ↓ (requestAnimationFrame 驱动)
按 currentTime 查找帧 → callback(batch) → 同一 hook → 同一 ECharts
```

### 6.4 API 总览

| 操作 | API | 模式 |
|------|-----|------|
| 订阅 SSE 流 | `POST /api/v1/events/subscribe` → `GET /api/v1/events/{id}` | Live |
| 更新订阅列表 | `POST /api/v1/events/{id}/update` | Live |
| 启动 Feature | `POST /api/v2/features/:name/start` | Live |
| 停止 Feature | `POST /api/v2/features/:name/stop` | Live |
| 查询 Feature 列表 | `GET /api/v2/features` → `{"features": [...]}` | Live |
| 开始后端录制 | `POST /api/v1/features/:name/record/start` | Live |
| 停止后端录制 | `POST /api/v1/features/:name/record/stop` | Live |
| 前端保存 buffer | (无 API，`useSaveBuffer()` 导出 DataBus ringBuffer) | Live |
| — | (无后端 API) | Replay |

---

## 七、资源控制

### 7.1 后端预算配置

```yaml
# illuminator.yaml
resource_budget:
  max_cpu_pct: 5              # Illuminator 自身最大 CPU 开销
  max_memory_mb: 200          # 最大内存占用
  max_ebpf_probes: 8          # 同时运行的 eBPF 探针数
  max_recording_mb: 500       # 单次录制最大大小
  auto_stop_on_leave: false   # 离开标签页是否自动停止
```

### 7.2 前端资源指示器

右上角始终显示当前资源开销状态：

```
[●] Overhead: 2.1% CPU | 45MB | Probes: 3
```

颜色指示：
- 绿色 (< 50% 预算)：正常
- 黄色 (50-80% 预算)：注意
- 红色 (> 80% 预算)：接近上限

---

## 八、技术栈

| 层面 | 选择 | 理由 |
|------|------|------|
| 框架 | React 18 + TypeScript | 组件化、类型安全 |
| 状态 | Zustand | 轻量、无 boilerplate |
| 路由 | React Router v6 | lazy loading |
| 图表 | Apache ECharts 5.5 | Canvas 高性能、丰富图表 |
| 火焰图 | 自定义 SVG | 无通用库满足需求 |
| 构建 | Vite | 快速 HMR |
| 测试 | Vitest | 与 Vite 集成 |

---

## 九、通信策略：SSE + HTTP 双通道

### 9.1 策略

```
┌─ 前端通信层 ──────────────────────────────────────────────────────┐
│                                                                    │
│  ┌─ SSE (主通道) ──────────────────────────────────────────────┐   │
│  │  订阅: POST /api/v1/events/subscribe                         │   │
│  │  连接: GET /api/v1/events/{id}  (EventSource)                │   │
│  │  封装: SseLink — EventSource wrapper + 自动重连               │   │
│  │  路由: DataBus — 管理订阅、ringBuffer、分片重组               │   │
│  │  事件: "data" (完整 payload) / "frame" (大 payload 分片)     │   │
│  │  优势: 单向推送、浏览器原生支持、无需轮询                     │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                    │
│  ┌─ HTTP REST (辅通道) ────────────────────────────────────────┐   │
│  │  用途: Feature 控制 (start/stop)、Feature 发现、录制控制      │   │
│  │  订阅管理: POST /api/v1/events/{id}/update 动态增删 Feature   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### 9.2 自动重连

```
SSE 连接成功 → SseLink.onOpen → DataBus 状态 "connected"
SSE 断开     → SseLink 指数退避重连 (1s → 2s → 4s → ... → 30s max)
重连成功     → 自动恢复 SSE 推送 + 隐藏 "连接中断" 提示
```

### 9.3 Page Visibility 感知

| 页面状态 | 行为 |
|---------|------|
| 前台可见 | SSE 正常接收 |
| 后台标签（不可见） | SSE 暂停订阅 / 降频消费 |
| 浏览器最小化 | 完全暂停前端数据消费（后端继续运行不受影响） |
| 回到前台 | 自动恢复 + 拉取最近数据补充 gap |

实现：`document.addEventListener('visibilitychange', ...)`

---

## 十、前端性能优化策略

### 10.1 Web Worker 隔离计算密集任务

以下操作移到 Worker 线程，避免阻塞 UI：

| 任务 | 复杂度 | Worker 必要性 |
|------|--------|-------------|
| `buildFlameTree()` 构建火焰图 | O(n × depth), n=2000 | 必须 |
| Replay .ilr 文件解析 | 数 MB 文件解析 | 必须 |
| `extractTopFunctions()` | O(n) 遍历 | 可选 |
| ECharts option 计算 | O(n) | 不需要（n 小） |

```
主线程                      Worker 线程
────────                    ────────────
postMessage(samples)  →     buildFlameTree(samples)
                      ←     postMessage(root)
setRoot(root) → 渲染
```

### 10.2 渲染优化

| 技术 | 应用场景 | 效果 |
|------|---------|------|
| **IntersectionObserver** | 不可见图表暂停更新 | 减少 50%+ 无效 Canvas 绑定 |
| **ECharts appendData** | 时序图追加新数据点 | 避免全量 setOption |
| **虚拟滚动** | 进程列表 > 50 行 | DOM 节点从 N 降到 ~20 |
| **React.memo** | 不变的子组件 | 跳过无效 re-render |
| **requestIdleCallback** | 非关键 UI 更新 | 保证 60fps 的主路径不阻塞 |

### 10.3 内存管理

```
Ring Buffer 策略:
  - 时序数据: 最多保留 120 个点 (2 分钟 @ 1Hz)
  - Profile 累积: 最多 2000 样本（可配置）
  - 进程列表: 最新一帧（不累积）

清理机制:
  - 标签页卸载时释放该页独占的 DataSource 订阅
  - AbortController: 每次 fetch 绑定，组件卸载自动取消
  - WeakRef: ECharts 实例引用，GC 可回收
```

---

## 十一、专业性能分析功能

### 11.1 Diff 火焰图

对比两次 Profile 的热点变化，是性能优化的核心工具：

```
┌─ Diff Flame Graph ────────────────────────────────────────────┐
│                                                                │
│  Baseline: session_v1.2.ilr (2026-06-10)                       │
│  Current:  session_v1.3.ilr (2026-06-12)                       │
│                                                                │
│  █████████████████████████████████  (红色 = 新增热点)          │
│  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  (蓝色 = 消除的热点)        │
│  ███████████████                    (灰色 = 无变化)            │
│                                                                │
│  Top Regressions:         Top Improvements:                    │
│  1. +5.2% new_serialize   1. -8.1% old_copy_loop             │
│  2. +3.1% mutex_lock      2. -4.2% redundant_alloc           │
└────────────────────────────────────────────────────────────────┘
```

**实现方式**：
- 两个 FlameTree 按栈路径对齐
- 计算每个节点的 diff (current_pct - baseline_pct)
- 红蓝色彩编码（红=回归，蓝=优化）
- 可用于 Live 模式两个时段对比，也可用于两个 Replay 文件对比

### 11.2 火焰图交互增强

| 功能 | 交互 | 实现 |
|------|------|------|
| **搜索** | 输入函数名 → 所有匹配帧高亮 + 显示累计占比 | 正则过滤 + CSS 半透明 |
| **Zoom** | 点击帧 → 以该帧为 root 放大 | 重建子树、面包屑导航 |
| **Tooltip** | hover → 显示函数名、sample 数、占比 | 固定或浮动 tooltip |
| **右键菜单** | 复制函数名、跳转源码、过滤该栈 | context menu |

### 11.3 火焰图时间选择与聚合

用户可以通过 Timeline 图表选择时间点或拖拽时间范围，火焰图仅聚合显示该范围内的样本。

**交互模式：**

| 状态 | Timeline 行为 | 火焰图聚合范围 |
|------|--------------|---------------|
| **Live** | 实时滚动、不可选择 | 当前 buffer 内全部 samples |
| **Paused + 点击** | 高亮选中点 | 选中时刻 ±500ms (1s window) |
| **Paused + 拖拽** | Brush 选择区间 | 选中起止范围内 samples |

**数据流：**

```
Timeline (ECharts brush/click)
    ↓ onTimeSelect(TimeSelection)
ProcessDetailView 状态管理
    ↓ timeSelection prop
ProfileSnapshot
    ↓ filtered = samples.filter(s => s.timestamp ∈ [start, end])
buildFlameTree(filtered)
    ↓
FlameGraph 渲染
```

**内存管理：**

- 每个 ProfileSnapshot 保持最大 5000 条带时间戳的 samples
- 超出上限时淘汰最旧的 samples (FIFO)
- 暂停时停止数据拉取，避免后台无限增长
- 所有 samples 存储在 `useRef` 中，不触发不必要的 re-render

**前端实现：**

```typescript
interface TimeSelection {
  type: 'point' | 'range'
  start: number  // epoch ms
  end: number    // epoch ms
}

// ProcessCpuTimeline 暂停时自动启用 brush 工具
// click 事件: { type: 'point', start: ts-500, end: ts+500 }
// brush 事件: { type: 'range', start: startTs, end: endTs }
```

### 11.4 跨图表时间联动

同一页面内多个图表共享时间轴游标：

```
┌─ CPU Utilization ──────────────────────────────────────────────┐
│  ___/\___/\___│___/\___     ← 垂直虚线（跟随鼠标/联动）       │
└───────────────┼────────────────────────────────────────────────┘
                │
┌─ IO Latency ──┼────────────────────────────────────────────────┐
│  ___/\___/\___|___/\___     ← 同一时间点对齐                   │
└───────────────┼────────────────────────────────────────────────┘
                │
         此时刻 CPU 和 IO 同时飙升 → 可能存在因果关系
```

实现：ECharts `connect` API 或自定义 EventBus 广播 hover timestamp。

### 11.5 URL State 持久化

```
/cpu?tab=process&pid=1234&comm=nginx&profile=on_cpu&mode=paused&t=1718150400000-1718150460000

URL 编码内容:
  - 当前子标签 (tab=system|process)
  - 选中的进程 PID 和进程名
  - Profile 类型
  - 时间模式 (mode=paused)
  - 时间范围 (t=start-end, epoch ms)
```

用途：
- 浏览器后退/前进按钮正常工作
- 复制 URL 分享给同事，对方看到完全相同的视图
- 书签收藏特定分析视图

**实现**：`useUrlState` hook 封装 `useSearchParams`，在页面组件中集成：
- 切换子标签时更新 `tab` 参数
- 选择进程时更新 `pid` + `comm` 参数
- 暂停时自动同步时间范围到 URL
- 页面加载时从 URL 恢复状态

### 11.6 导出功能

| 导出类型 | 格式 | 用途 |
|---------|------|------|
| 火焰图 | SVG / PNG | 嵌入报告、演示 |
| 时序数据 | CSV / JSON | 外部工具分析 |
| Profile 数据 | folded format / pprof | 兼容 FlameGraph.pl / go tool pprof |
| 当前视图 | 截图 (html2canvas) | 快速分享 |

---

## 十二、错误处理与优雅降级

### 12.1 连接状态管理

```
┌─ 右上角状态指示器 ─────────────────────────────────────────────┐
│                                                                │
│  正常:     [●] Connected | Overhead: 2.1% CPU                  │
│  断开:     [○] Disconnected (retrying in 3s...)                │
│  后端崩溃: [✕] Backend unreachable                             │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

### 12.2 降级策略

| 场景 | 行为 |
|------|------|
| SSE 断开 | SseLink 自动重连 + 显示 "连接中断" 提示 |
| 后端未响应 | 图表冻结在最后一帧 + "无数据" 水印 |
| Feature 启动失败 | 显示错误原因 + "重试" 按钮 |
| eBPF 不可用 | Tier 3 按钮禁用 + tooltip 说明原因 |
| 录制磁盘满 | 自动停止录制 + 通知 |

### 12.3 自动重连

```
断开 → SseLink 指数退避: 1s → 2s → 4s → 8s → 16s → 30s (max)
重连成功 → EventSource 重新建立 → DataBus 恢复接收 SSE 事件
```

---

## 十三、性能预算

| 指标 | 目标 | 验证方式 |
|------|------|---------|
| 首屏加载 | < 500ms | Lighthouse |
| 标签页切换 | < 100ms | React Profiler |
| 图表帧率 | ≥ 30fps (持续) | Performance Monitor |
| 火焰图渲染 (2000 samples) | < 50ms | console.time |
| 单页 JS bundle | < 150KB gzip | vite-bundle-visualizer |
| 前端内存峰值 | < 100MB (长时间运行) | Memory DevTools |
| SSE → 图表延迟 | < 100ms | 端到端计时 |
| Replay 文件加载 (50MB) | < 3s | Worker 计时 |

---

## 十四、实现路线图

| Phase | 内容 | 优先级 |
|-------|------|--------|
| **Phase 1** | Overview + CPU 标签页 (自定义 SVG) | ✅ 完成 |
| **Phase 2** | ECharts 迁移 + SSE 推送 + 分层自动激活 + Page Visibility + 资源预算 | ✅ 完成 |
| **Phase 3** | 火焰图增强 (搜索 + Zoom + Diff) + Web Worker | ✅ 完成 |
| **Phase 4** | Memory 标签页 | ✅ 完成 |
| **Phase 5** | IO + Network 标签页 | ✅ 完成 |
| **Phase 6** | 全局录制 + Replay 模式 | ✅ 完成 |
| **Phase 7** | 跨图表时间联动 + URL State + 导出 | ✅ 完成 |
| **Phase 8** | GPU 标签页 | ✅ 完成 |
| **Phase 9** | 插件热加载管理面板 + 后端热加载 API | ✅ 完成 |
| **Phase 10** | DataSource 统一 + Replay 图表集成 | ✅ 完成 |
| **Phase 11** | 虚拟滚动 — 进程列表 > 40 行自动虚拟化 | ✅ 完成 |
| **Phase 12** | 核心 hooks 单元测试 (useCpuData, ReplayEngine) | ✅ 完成 |
| **Phase 13** | Annotations/Markers 时间轴标注系统 | ✅ 完成 |

### 待完成项

| 项目 | 说明 | 优先级 |
|------|------|--------|
| Replay 标签页完善 | IO/Network/GPU 的 Replay 视图实现（当前为 placeholder） | P1 |
| Brush 时间范围 → 火焰图过滤 | 拖拽选择后联动火焰图聚合已实现，需更多端到端验证 | P1 |
| SSE 大 payload 分片优化 | frame 分片重组已实现，需更多端到端验证 | P2 |
| 前端性能优化 | React.memo + requestIdleCallback + ECharts appendData | P2 |
| System 页面完善 | 调度延迟直方图 + 中断统计 + 运行队列详细分析 | P2 |
| 告警规则系统 | CPU/内存超阈值自动触发 Annotation + 浏览器通知 | P3 |
| Replay 近期文件列表 | 记住上次打开的 .ilr 文件，方便快速重载 | P3 |
| useProcessDetail 单元测试 | processGone 逻辑和 timeline 连续性的测试覆盖 | P3 |

---

## 十五、插件热加载架构

### 15.1 后端热加载

```
用户点击 "Hot Reload Plugins" 按钮
    ↓
POST /api/v1/plugins/reload
    ↓
PluginManager::LoadPluginsFromDirs(allowed_dirs)
    ↓ 扫描 plugin_dirs/ 中所有 .so 文件
SoLoader::LoadPlugin(path)  → dlopen() + 获取 IlPluginDescriptor
    ↓
BridgeDescriptorToRegistry → 注册到 PluginRegistry
    ↓
返回新增插件列表给前端
```

**设计要点**：
- 新增的 .so 插件无需重启即可生效
- 已加载的插件不会被卸载（避免正在使用的 pipeline 崩溃）
- PluginRegistry 线程安全，支持运行时新增
- Feature 定义可热更新（通过 YAML 配置重载）

### 15.2 前端插件管理面板

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Plugin & Feature Manager              [Hot Reload Plugins]              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌── Resource Budget ──────────────────────────────────────────────┐    │
│  │  CPU: 2.1% / 5%  │  Memory: 45MB / 200MB  │  Probes: 3 / 8   │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                         │
│  [All(12)] [Active(5)] [Inactive(7)]  │  🔥cpu  🧠memory  💾io ...     │
│                                                                         │
│  ┌─ Feature Card ─────────────────────────────────────────────────┐    │
│  │  🔥 CPU Utilization                          [ACTIVE]          │    │
│  │     cpu_utilization | Tier 1 · Monitoring | cpu                │    │
│  │     Batches: 1,234  Records: 5,678  Errors: 0                 │    │
│  │     Uptime: 2m 15s                                            │    │
│  │     [Pause] [Stop]                                            │    │
│  └────────────────────────────────────────────────────────────────┘    │
│                                                                         │
│  ┌─ Action Log ───────────────────────────────────────────────────┐    │
│  │  14:32:15 ✓ start cpu_utilization                              │    │
│  │  14:32:12 ✓ Plugins reloaded successfully                      │    │
│  └────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

### 15.3 API

| 操作 | API | 说明 |
|------|-----|------|
| 查询已注册插件 | `GET /api/v1/plugins` | 列出所有 source/processor/aggregator/sink 插件 |
| 热加载插件 | `POST /api/v1/plugins/reload` | 重新扫描 plugin_dirs 并加载新 .so |
| Feature 启动 | `POST /api/v2/features/:name/start` | 创建并启动对应 pipeline |
| Feature 停止 | `POST /api/v2/features/:name/stop` | 销毁 pipeline 释放资源 |
| Feature 暂停 | `POST /api/v2/features/:name/pause` | 暂停采集但保留 pipeline |
| Feature 恢复 | `POST /api/v2/features/:name/resume` | 从暂停恢复采集 |

### 15.4 导出功能

| 导出类型 | 格式 | 实现 |
|---------|------|------|
| 时序数据 | CSV | `exportTimeSeriesCSV()` — 所有字段作为列 |
| 时序数据 | JSON | `exportTimeSeriesJSON()` — 完整时间戳数据 |
| 火焰图 | Folded Stacks | `exportFoldedFormat()` — 兼容 FlameGraph.pl |
| 图表截图 | PNG | `exportEChartAsPNG()` — Canvas 导出 |
| 当前视图 | PNG | `exportCurrentView()` — html2canvas 全页截图 |

---

## 十六、DataSource 统一与 Replay 集成

### 16.1 DataSource 全局单例（实际实现）

> **注意**: 原设计中的 `DataSourceContext` / `DataSourceProvider` 已废弃删除。
> 实际采用的是 `getDataSource()` 全局单例模式，无 Provider 嵌套开销。

```
┌─ getDataSource() → dataBus 单例 (DataBus) ──────────────────────┐
│                                                                  │
│  SSE 主通道: POST /api/v1/events/subscribe                       │
│              → GET /api/v1/events/{id} (SseLink / EventSource)   │
│  数据路由: DataBus.subscribe(feature, cb) → ringBuffer + 回调    │
│  自动重连: SseLink 指数退避 (1s → 30s)                           │
│                                                                  │
│  Replay: 各 hook 通过可选 replaySource?: DataSource 参数覆盖     │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

核心类：
- **`DataBus`**（`web/src/services/dataBus.ts`）— SSE 订阅管理、数据路由、ringBuffer
- **`SseLink`**（`web/src/services/sseLink.ts`）— EventSource 封装，自动重连

### 16.2 Hook 双模式设计

数据 hooks 支持两种工作方式：

```typescript
// Live 模式（默认）— 通过 DataBus SSE 订阅
useCpuUtilization(true)

// Replay 模式 — 通过 DataSource 订阅
useCpuUtilization(true, replayEngine)
```

通过可选的 `replaySource` 参数，hooks 内部切换数据获取逻辑：
- 有 `replaySource`：订阅 ReplayEngine 的 feature 频道
- 无 `replaySource`：`getDataSource().subscribe('cpu_utilization', cb)` → DataBus SSE 推送

SSE payload 经 `extractRecords(batch.data)` 解析后供各 hook 消费。

### 16.3 Replay 页面功能

Replay 页面加载 `.ilr` 文件后：
1. 自动检测可用 features（cpu/memory/io/network/gpu）
2. 显示对应的功能标签页
3. 复用 Live 模式的图表组件（共享 ECharts 配置和渲染逻辑）
4. 底部提供播放控制栏（播放/暂停/seek/速度调节）

---

## 十七、虚拟滚动

### 17.1 触发条件

进程列表行数 > 40 时自动启用虚拟化渲染：

| 项目 | 值 |
|------|-----|
| 行高 | 37px 固定 |
| 最大可视高度 | 600px |
| 过扫描缓冲 | ±5 行 |
| 阈值 | 40 行 |

### 17.2 实现方式

- 无外部依赖，纯内联虚拟化
- 使用 spacer `<tr>` 维持正确滚动高度
- `onScroll` 计算可视窗口，仅渲染可见行 + overscan
- 低于阈值时回退到普通 DOM 渲染（零开销）

---

## 十八、Annotations 标注系统

### 18.1 用途

在时间轴图表上标记关键事件，辅助性能分析的因果推断：

| 类型 | 图标 | 典型用途 |
|------|------|---------|
| deploy | 🚀 | 版本部署时刻 |
| alert | ⚠ | 告警触发 |
| spike | ⚡ | CPU/内存突刺 |
| gc | 🗑 | GC 事件 |
| manual | 📌 | 用户手动标记 |

### 18.2 交互

- TimeControls 栏右侧提供 "+ Mark" 按钮
- 点击后展开内联表单：选择类型 + 输入标签
- 标注以竖线 + 图标形式叠加在图表时间轴上
- Hover 显示 tooltip（标签 + 时间 + 描述）

### 18.3 数据管理

`useAnnotationStore`（Zustand）管理全局标注列表：
- `add(ann)`: 添加标注
- `remove(id)`: 删除标注
- `getInRange(start, end)`: 按时间窗口过滤

标注数据生命周期与页面会话绑定，后续可扩展为后端持久化。
