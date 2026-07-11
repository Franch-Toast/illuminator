# Illuminator 项目入门指南

> **文档目的**：为首次接触本项目的开发者提供系统性的学习路径，帮助快速理解项目架构、核心概念和代码组织。
>
> **适用人群**：C++ 后端开发者、前端开发者、系统工程师、对 eBPF/可观测性感兴趣的开发者
>
> **预计阅读时间**：30 分钟精读 + 2-3 天实操探索
>
> **更新日期**：2026-07-02

---

## 目录

1. [项目是什么](#1-项目是什么)
2. [10 分钟快速上手](#2-10-分钟快速上手)
3. [后端学习路线图](#3-后端学习路线图)
4. [前端学习路线图](#4-前端学习路线图)
5. [核心概念详解](#5-核心概念详解)
6. [代码阅读顺序（文件级指引）](#6-代码阅读顺序文件级指引)
7. [如何新增一个插件（端到端示例）](#7-如何新增一个插件端到端示例)
8. [如何新增一个前端页面](#8-如何新增一个前端页面)
9. [测试体系导航](#9-测试体系导航)
10. [常见问题与陷阱](#10-常见问题与陷阱)
11. [未来展望与开发路线图](#11-未来展望与开发路线图)

---

## 1. 项目是什么

Illuminator 是一个 **高性能、插件化的全栈可观测性平台**。它通过 eBPF 技术在 Linux 上进行零侵入数据采集（CPU、调度器、I/O、网络），配套自研 Web 可视化平台实时展示，通过 SSE（Server-Sent Events）推送实时数据，REST API 负责控制面。

**一句话总结**：`eBPF 数据采集 → FeatureDriver Pipeline → SinkPool → SSE 实时推送 → React DataBus → 可视化`

### 技术栈概览

| 层级 | 技术选择 | 理由 |
|------|---------|------|
| 语言 | C++20 | 性能敏感、系统级编程、concept/coroutine 支持 |
| 构建 | Bazel (bzlmod) | 可重复构建、依赖管理、BPF 编译规则 |
| 数据采集 | eBPF (libbpf + CO-RE) | 零侵入、内核级可观测 |
| 日志 | spdlog | 高性能、fmt 风格 |
| 配置 | yaml-cpp | 人类友好、结构化 |
| 序列化 | nlohmann/json | Header-only、易用 |
| HTTP | cpp-httplib | 单头文件、轻量 |
| 实时推送 | SSE (Server-Sent Events) | 单连接多 Feature 订阅、自动重连 |
| 存储 | SQLite (WAL) | 嵌入式、零运维 |
| 前端框架 | React 18 + TypeScript | 现代 SPA、类型安全 |
| 前端构建 | Vite 5 | 快速 HMR、ESM 原生 |
| 图表 | ECharts 6 (tree-shaken) | 高性能、丰富图表类型 |
| 火焰图 | Web Worker + CSS div | 异步计算、主线程不阻塞 |
| 状态管理 | Zustand 5 | 轻量、无 Provider 嵌套 |
| 前端测试 | Vitest + Testing Library | 快速、兼容 Jest |

---

## 2. 10 分钟快速上手

### 2.1 环境准备

```bash
# 安装系统依赖 (Ubuntu 22.04+)
sudo apt install -y clang llvm libbpf-dev libelf-dev zlib1g-dev libsqlite3-dev

# 安装 Bazel (推荐 Bazelisk)
sudo npm install -g @bazel/bazelisk

# 安装前端依赖
cd illuminator/web && npm install
```

### 2.2 构建与运行

```bash
cd illuminator

# 构建后端
bazel build //src/cli:illuminator

# 运行后端测试
bazel test //src/...

# 启动后端守护进程（需要 root，因为 eBPF；已默认开启 Always-On）
sudo ./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example

# 另一个终端：启动前端开发服务器（Vite HMR 会自动代理 API 到 9527 端口）
cd web && npm run dev    # → 访问 http://localhost:3000
```

### 2.3 验证

```bash
# 后端健康检查
curl http://localhost:9527/healthz

# 查看已注册的 Feature（Always-On 模式下 Tier 1-2 自动运行）
curl http://localhost:9527/api/v2/features
# → 所有 tier<=2 的 feature 应为 "active" 状态

# 注册 SSE 订阅（数据面）
curl -X POST http://localhost:9527/api/v1/events/subscribe \
  -H 'Content-Type: application/json' \
  -d '{"features":["cpu_utilization"]}'
# → 返回 subscription_id 和 SSE 连接 URL

# 按需启动 Tier 3 profiling（FeatureBus 控制面）
curl -X POST http://localhost:9527/api/v2/features/cpu_profiler/start
curl -X POST http://localhost:9527/api/v2/features/cpu_profiler/stop
```

### 2.4 访问界面

| 地址 | 说明 |
|------|------|
| `http://localhost:3000` | 前端开发服务器（Vite HMR，代理到后端） |
| `http://localhost:9527` | 后端直接访问（静态文件 + API） |
| `http://localhost:9527/healthz` | 健康检查（无认证） |
| `http://localhost:9527/metrics` | Prometheus 指标（无认证） |
| `POST /api/v1/events/subscribe` | SSE 订阅注册（返回 subscription_id） |
| `GET /api/v1/events/{id}` | SSE 长连接，推送已订阅 Feature 的数据 |

---

## 3. 后端学习路线图

### 阶段一：理解数据流（Day 1）

**目标**：理解"数据从哪来、到哪去"

```
Source (数据源) → AsyncChannel → Processor (处理器) → Aggregator (聚合器) → Sink (输出端)
```

**必读文件**：
1. `src/core/common/data_batch.h` — 数据的"容器"长什么样
2. `src/plugin/api/source_plugin.h` — Pull 模式 vs Push 模式
3. `src/plugin/api/sink_plugin.h` — 数据最终去向

**实验**：
```bash
# daemon 启动后 Tier 1-2 自动运行，查看 Feature 列表
curl http://localhost:9527/api/v2/features | python3 -m json.tool

# 注册 SSE 订阅并建立长连接（前端 DataBus 的底层协议）
curl -X POST http://localhost:9527/api/v1/events/subscribe \
  -H 'Content-Type: application/json' \
  -d '{"features":["cpu_utilization"]}'
```

### 阶段二：理解线程模型（Day 1-2）

**目标**：理解 Pipeline v3 事件驱动架构的线程分工

```
TimerWheel (1 线程)       — 纯调度，不执行实际工作
    ├→ CollectPool (M 线程)  — 并行执行 Source::Collect()
    └→ InjectFlush          — 注入 FlushSentinel 到 channel

AsyncChannel (无锁队列)    — MPSC，variant<DataBatch, FlushSentinel>
    └→ ProcessThread (每管道 1)  — 纯事件处理器
         ├ DataBatch → processors → aggregator.Add → sinks
         └ FlushSentinel → aggregator.Flush → sinks

SinkPool (K 线程)          — 并行执行 Sink::Write()
```

**必读文件**：
1. `docs/pipeline_v3_design.md` — 架构设计文档（重点看前 200 行）
2. `src/core/engine/infrastructure_manager.h` — TimerWheel + CollectPool + SinkPool
3. `src/core/engine/feature_driver.h` — 每个 Feature 自包含的 Pipeline 构建
4. `src/core/engine/pipeline.h` — `Pipeline` 类的 `Start()` 和 `ProcessLoop()`

### 阶段三：理解 FeatureBus / FeatureDriver 与 Always-On 架构（Day 2）

**目标**：理解 Feature 生命周期管理和数据推送到前端的完整路径

#### Always-On 模式

```
daemon 启动
├── InfrastructureManager 启动（TimerWheel + CollectPool + SinkPool）
├── FeatureRegistry::RegisterAll() 注册所有 FeatureDriver
├── FeatureBus::ProbeAll() 自动 Start Tier 1 (Monitoring) + Tier 2 (Tracing)
│   └── 无需用户干预，打开浏览器即有数据
└── Tier 3 (Profiling) 通过 /api/v2/features/:name/start 按需启动
```

#### 架构组件职责

```
InfrastructureManager (共享基础设施)
├── TimerWheel — 全局定时调度
├── CollectPool — 并行执行 Source::Collect()
└── SinkPool — 并行执行 Sink::Write()（含 SseSink 推送）

FeatureBus (注册与生命周期编排)
├── 维护已注册 FeatureDriver 列表
├── Probe / Remove / Pause / Resume 单个或全部 Driver
├── 提供 /api/v2/features 控制面 API
└── 状态变更通知（SSE 推送用）

FeatureDriver (每个 Feature 自包含)
├── 声明元数据（Name, DisplayName, Category, Tier）
├── BuildPipeline() 构建 Source → Processor → SseSink 链
├── Probe() 启动 Pipeline 并注册定时器
└── Remove() 停止 Pipeline 并释放资源
```

#### 数据推送到前端全路径

这是整个项目中最关键的数据通路。理解这条路径就掌握了前后端通信的核心：

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         BACKEND (C++, port 9527)                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  FeatureDriver::BuildPipeline()                                         │
│  eBPF Source ─→ AsyncChannel ─→ Processor ─→ Aggregator                 │
│                                                       │                 │
│                                                       ▼                 │
│                                              SseSink (per feature)      │
│                                                       │                 │
│                                                       ▼                 │
│                              SseHandler::Push() (SinkPool 线程)         │
│                              ┌ per-subscription outbox queue ──┐       │
│                              │                                  │       │
│              ┌───────────────┼──────────────┐                   │       │
│              ▼               ▼              ▼                   │       │
│   POST /events/subscribe  GET /events/{id}  POST /events/{id}/update    │
│   (注册订阅)              (SSE 长连接)      (动态更新订阅)              │
│              │               │                                          │
└──────────────┼───────────────┼──────────────────────────────────────────┘
               │               │
               ▼               ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                    FRONTEND (React, DataBus)                              │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  DataBus (全局单例, 实现 DataSource 接口)                                 │
│  ├── connect() → POST /api/v1/events/subscribe → 获取 subscription_id    │
│  ├── SseLink: EventSource 连接 GET /api/v1/events/{id}                   │
│  │   └── 收到 data/frame 事件 → 解析 JSON → 推断 modelType → DataBatch   │
│  ├── subscribe(feature, cb) → 动态更新订阅列表                           │
│  └── 本地 ringBuffer 缓存最近 N 条（前端导出 .ilr 用）                     │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ useCpuData / useIoData / useGpuData / ... (各页面 hook)            │  │
│  │   └── getDataSource().subscribe(featureName) → DataBatch → state   │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ useFeatureHealth(featureName) — 监测数据到达间隔                    │  │
│  │   ├── < 5s → "active"                                              │  │
│  │   ├── 5-15s → "degraded"                                           │  │
│  │   └── > 15s → "unavailable"                                        │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

#### 前端 UI 分层模型

```
┌─────────────────────────────────────────────────────┐
│  Always-On Monitoring (Tier 1-2)                    │
│  ────────────────────────────────────               │
│  用户打开页面 → 数据自动流入 → 图表实时更新          │
│  无按钮、无等待                                      │
│  例：CPU Timeline, Memory Chart, IO Throughput       │
├─────────────────────────────────────────────────────┤
│  On-Demand Profiling (Tier 3)                       │
│  ────────────────────────────────────               │
│  用户点击 "Start Profile" → POST /api/v2/features/cpu_profiler/start    │
│  → 数据采集 → 火焰图渲染 → 会话自动过期或手动停止   │
│  例：On-CPU Flame Graph, Off-CPU Analysis           │
├─────────────────────────────────────────────────────┤
│  Offline Replay                                     │
│  ────────────────────────────────────               │
│  用户上传 .ilr 文件 → ReplayEngine 流式解析         │
│  → 同一 UI 组件渲染历史数据                         │
│  Export API: RecordingSink / DataBus ringBuffer → .ilr → 可回放          │
└─────────────────────────────────────────────────────┘
```

**必读文件**：
1. `src/core/engine/infrastructure_manager.h` — 共享基础设施（TimerWheel + CollectPool + SinkPool）
2. `src/core/engine/feature_bus.h` — FeatureDriver 注册与生命周期编排
3. `src/core/engine/feature_driver.h` — 自包含 Pipeline 构建与状态机
4. `src/plugin/features/feature_registry.h` — `REGISTER_FEATURE` 宏自动注册
5. `src/server/sse_handler.h` — SSE 订阅模型 + SseSink 推送
6. `src/server/api_routes.h` — REST API + Recording API
7. `web/src/services/dataBus.ts` — 前端 SSE 数据总线
8. `web/src/services/sseLink.ts` — EventSource 封装与自动重连

### 阶段四：理解配置系统（Day 2）

**目标**：理解 YAML 如何驱动 daemon 的服务端配置

```
illuminator.yaml.example / 内置 kDefaultConfigYaml
    └→ YamlConfigLoader::LoadFromString/File()
        └→ GlobalConfig { engine, server{auth_token}, ... }
            ├→ InfrastructureManager::Start(engine 线程池配置)
            └→ FeatureRegistry::RegisterAll() + FeatureBus::ProbeAll()
                （Feature 由 REGISTER_FEATURE 宏注册，非 YAML pipelines 驱动）

注：`collect` CLI 子命令同样使用 FeatureBus::ProbeAll() + sleep + RemoveAll()。
```

**必读文件**：
1. `illuminator.yaml.example` — 完整配置参考
2. `src/core/common/config.h` — `ConfigValue`, `PipelineConfig`, `GlobalConfig` 三层结构
3. `src/core/common/yaml_config_loader.h` — YAML → GlobalConfig 的转换逻辑

### 阶段五：理解插件系统（Day 2-3）

**必读文件**：
1. `src/plugin/manager/plugin_registry.h` — 宏注册机制 `IL_REGISTER_*`
2. `src/plugin/sources/cpu/cpu_utilization/cpu_utilization.h` — 最简单的 Pull Source
3. `src/plugin/processors/filter/filter_processor.h` — 标签过滤 Processor
4. `src/plugin/sinks/console_output/console_sink.h` — 最简单的 Sink

### 阶段六：eBPF 子系统（专项）

**前置知识**：BPF 基础、libbpf 用法

**必读文件**：
1. `src/ebpf/include/event_types.h` — 内核/用户态共享数据结构
2. `src/plugin/sources/ebpf_skeleton_source.h` — eBPF Skeleton Push Source 基类（bpftool gen skeleton）
3. `src/ebpf/probes/bpf_probe.bzl` — BPF 编译与 skeleton 生成 Bazel 规则
4. `src/ebpf/probes/cpu/cpu_profiler.bpf.c` — BPF C 程序示例
5. `src/plugin/sources/cpu/cpu_profiler/cpu_profiler.h` — 复杂 skeleton 源示例

---

## 4. 前端学习路线图

### 阶段一：整体结构（1 小时）

**目标**：理解前端的文件组织和路由

```
web/src/
├── App.tsx                    路由 (10 页面, React.lazy + Suspense)
├── main.tsx                   挂载点
├── components/
│   ├── charts/                图表组件 (ECharts + FlameGraph)
│   ├── shared/                共享组件 (SummaryCard/Sparkline/EmptyChart)
│   └── Layout/                布局组件 (Sidebar/ExportControl/ConnectionIndicator)
├── services/
│   ├── apiClient.ts           REST API 客户端（/api/v2/features 控制面）
│   ├── dataSource.ts          DataSource 接口 + DataBatch (含 modelType)
│   ├── dataBus.ts             SSE 数据总线（全局单例）
│   └── sseLink.ts             EventSource 封装（自动重连、分帧重组）
├── hooks/                     数据 hooks（DataBus 驱动, 支持 replaySource）
├── utils/                     工具库 (TimeSeriesBuffer 等)
├── stores/                    Zustand 状态 (time/pipeline/annotation)
├── workers/                   Web Worker (火焰图异步计算)
└── pages/                     10 个懒加载页面
```

**必读文件**：
1. `web/src/App.tsx` — 路由、全局布局、DataBus 连接初始化
2. `web/src/services/dataSource.ts` — `DataSource` 接口 + `DataModelType` 定义
3. `web/src/services/dataBus.ts` — SSE 订阅管理、分帧重组、ringBuffer
4. `web/src/services/sseLink.ts` — EventSource 连接管理与自动重连
5. `web/src/utils/timeSeriesBuffer.ts` — 滑动窗口时间序列缓冲

### 阶段二：数据流（1 小时）

**目标**：理解前端如何获取和消费后端数据

```
                      ┌─ POST /api/v1/events/subscribe ───┐
getDataSource() ─────►│  注册 features → subscription_id  │
(全局 DataBus 单例)   │  GET /api/v1/events/{id} (SSE)    │
                      │  ← JSON 实时推送                   │
                      └───────────────────────────────────┘

数据 Hook 使用方式:
  const source = replaySource ?? getDataSource()
  source.subscribe('cpu_utilization', (batch) => {
    // 处理实时数据
  })
```

**必读文件**：
1. `web/src/hooks/useDataSource.ts` — 全局单例 + subscribe/连接状态
2. `web/src/hooks/useCpuData.ts` — 典型数据 hook 实现
3. `web/src/services/apiClient.ts` — REST API 客户端

### 阶段三：火焰图渲染（1 小时）

**目标**：理解性能敏感组件的实现

```
ProfileSnapshot.tsx:
  1. getDataSource().subscribe('cpu_profiler', cb) → SSE 接收 profile 数据
  2. 积累 StackSample[] (最多 5000)
  3. Worker.postMessage({type:'build', samples}) → 异步树构建
  4. Worker 返回 FlameNode 树 → 渲染为 flexbox div
```

**必读文件**：
1. `web/src/components/charts/ProfileSnapshot.tsx` — 数据获取 + 渲染
2. `web/src/workers/flameGraphWorker.ts` — 树构建算法

### 阶段四：状态管理与 UI（30 分钟）

**必读文件**：
1. `web/src/stores/useTimeStore.ts` — live/paused 模式切换
2. `web/src/components/Layout/StatusBar.tsx` — 连接状态显示
3. `web/src/components/charts/EChart.tsx` — ECharts 包装器

---

## 5. 核心概念详解

### 5.1 DataBatch — 数据的"容器"

```cpp
DataBatch
├── Type: kMetrics | kProfile | kTrace | kLog | kGeneric
├── Arena (shared_ptr) — 零拷贝内存池
├── Records[] — 指标数据 (CPU%, 内存, 负载...)
│   ├── Labels: {cpu: "0", type: "cpu_core"}
│   └── Fields: {busy_pct: 45.3, idle_pct: 54.7}
└── StackSamples[] — 性能剖析数据 (火焰图)
    ├── comm: "nginx", tid: 1234
    ├── kernel_stack: [{function_name: "do_syscall", address: 0x...}]
    ├── user_stack: [{function_name: "main", address: 0x...}]
    └── count: 42
```

### 5.2 DataBus — SSE 数据总线

```typescript
DataBus (全局单例, 实现 DataSource 接口)
├── connect()
│   ├── POST /api/v1/events/subscribe → 获取 subscription_id
│   └── SseLink.connect() → EventSource GET /api/v1/events/{id}
├── subscribe(feature, cb)
│   ├── 维护 per-feature callbacks + ringBuffer
│   └── syncSubscription() → POST /api/v1/events/{id}/update 动态更新
├── handleData / handleFrame
│   ├── 解析 SSE data 事件 → 推断 modelType → DataBatch
│   └── 分帧重组（>64KB 数据自动分帧传输）
└── SseLink 自动重连（指数退避 1s → 30s max）
```

**关键设计：** SseLink 负责传输层，DataBus 负责订阅管理和数据分发。现有页面 hooks 通过 `getDataSource()` 无缝接入。

### 5.3 Feature 生命周期（Always-On 模式）

```
系统事件                   →   行为                      →   说明
───────────────────────────────────────────────────────────────────
daemon 启动               →   Tier 1-2 全部自动 Probe   →   FeatureBus::ProbeAll()
                          →   数据经 SseSink 推送到 SSE  →   前端打开即有数据

用户打开 CPU 页面         →   subscribe("cpu_utilization")
                          →   DataBus 通过 SSE 接收数据
                          →   图表实时更新（无等待、无按钮）

用户点击 "Start Profile"  →   POST /api/v2/features/cpu_profiler/start
                          →   FeatureBus::Probe() → Tier 3 Pipeline 启动
                          →   Source per-CPU perf_event + BPF 内核态 tgid 过滤
                          →   数据经 SSE 推送 → 火焰图渲染

手动 Stop                 →   POST /api/v2/features/cpu_profiler/stop
                          →   FeatureBus::Remove() → Pipeline 停止 → 资源释放
                          →   DataBus ringBuffer 中已缓存数据仍可查看

用户点击 "Export"         →   Recording API 或 DataBus ringBuffer 导出
                          →   生成 .ilr 文件 → 前端下载
```

### 5.4 AsyncChannel — 异步通信

| 消息类型 | 用途 | 来源 |
|---------|------|------|
| `DataBatchPtr` | 正常数据批次 | Source::Collect() 或 Push callback |
| `FlushSentinel` | 触发 Aggregator flush | TimerWheel 周期注入 |

**反压机制**：队列使用率 > 80% 触发反压，< 20% 解除。
**丢弃策略**：`drop_newest`（保留历史）或 `drop_oldest`（保留最新）。

### 5.5 SSE 认证

SSE 订阅和控制面 API 均走 `/api/` 路径，通过 `SetupAuthMiddleware` 验证 Bearer Token：
- `Authorization: Bearer <token>` 请求头（POST subscribe 等 fetch 请求）
- 配置中 `server.auth_token` 为空时跳过验证（开发模式）

注：EventSource（GET SSE 长连接）无法自定义请求头；开发模式下通过 Vite proxy 同源访问即可。

---

## 6. 代码阅读顺序（文件级指引）

### 第一轮：建立心智模型（2 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 1 | `README.md` | 项目概览、构建说明 |
| 2 | `docs/pipeline_v3_design.md` (前 200 行) | v3 设计动机和架构决策 |
| 3 | `illuminator.yaml.example` | 配置全貌，理解系统能力边界 |
| 4 | `src/cli/main.cc` | 程序入口，daemon/collect/top 启动流程 |
| 5 | `docs/code_review_report.md` | 全栈架构审查，了解已完成的改进 |

### 第二轮：核心数据结构（1.5 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 6 | `src/core/common/status.h` | `StatusCode`/`Status::Wrap()`/`StatusOr<T>` |
| 7 | `src/core/common/config.h` | `ConfigValue`/`PipelineConfig`/`GlobalConfig` |
| 8 | `src/core/common/data_batch.h` | `Record`/`StackSample`/`DataBatch` + Arena |

### 第三轮：管道引擎（3 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 9 | `src/core/memory/arena.h` | bump-pointer 分块策略 |
| 10 | `src/core/memory/lock_free_queue.h` | MPSC CAS 环形缓冲区 |
| 11 | `src/core/engine/async_channel.h` | ChannelItem variant、反压水位线 |
| 12 | `src/core/engine/infrastructure_manager.h` | TimerWheel + CollectPool + SinkPool |
| 13 | `src/core/engine/feature_driver.h` | FeatureDriver 状态机、BuildPipeline |
| 14 | `src/core/engine/feature_bus.h` | 注册、Probe/Remove、ProbeAll |

### 第四轮：插件 & 服务层（2 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 15 | `src/plugin/features/feature_registry.h` | REGISTER_FEATURE 宏注册 |
| 16 | `src/plugin/features/cpu_utilization_driver.h` | 典型 Tier 1 FeatureDriver |
| 17 | `src/server/sse_handler.h` | SSE 订阅模型 + SseSink 推送 |
| 18 | `src/server/api_routes.h` | REST API 端点全景 |

### 第五轮：前端架构（2 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 20 | `web/src/App.tsx` | 路由、全局布局、SSE 连接 |
| 21 | `web/src/services/dataBus.ts` | SSE 数据总线核心 |
| 22 | `web/src/hooks/useDataSource.ts` | getDataSource() 全局单例 |
| 23 | `web/src/hooks/useCpuData.ts` | 典型数据 hook（subscribe 模式） |
| 24 | `web/src/services/apiClient.ts` | REST API 客户端（/api/v2/features） |
| 25 | `web/src/components/charts/ProfileSnapshot.tsx` | 火焰图：SSE 订阅 + Worker + 渲染 |
| 26 | `web/src/workers/flameGraphWorker.ts` | Worker 端：树构建算法 |

---

## 7. 如何新增一个插件（端到端示例）

以添加一个 **MemoryUsageDriver**（内存使用监控 Feature）为例。RFC v3 架构下，每个 Feature 是一个自包含的 FeatureDriver，而非 YAML pipeline 配置。

### Step 1: 创建 FeatureDriver

`src/plugin/features/memory_usage_driver.h`

```cpp
#pragma once
#include "core/engine/feature_driver.h"
#include "features/feature_registry.h"
#include "server/sse_handler.h"
#include "sources/memory/memory_usage/memory_usage.h"

namespace illuminator {

class MemoryUsageDriver : public FeatureDriver {
public:
    const char* Name() const override { return "memory_usage"; }
    const char* DisplayName() const override { return "Memory Usage"; }
    const char* Category() const override { return "memory"; }
    DriverTier Tier() const override { return DriverTier::kMonitoring; }

protected:
    std::unique_ptr<Pipeline> BuildPipeline(InfrastructureManager& infra) override {
        auto pipeline = std::make_unique<Pipeline>("memory_usage");

        auto source = std::make_unique<MemoryUsageSource>();
        ConfigValue cfg;
        cfg["interval_ms"] = ConfigValue(static_cast<int64_t>(2000));
        source->Init(cfg);

        pipeline->SetSource(std::move(source));
        pipeline->AddSink(std::make_unique<SseSink>("memory_usage"));
        return pipeline;
    }
};

REGISTER_FEATURE(MemoryUsageDriver);

}  // namespace illuminator
```

> Source 插件（`MemoryUsageSource`）仍使用 `IL_REGISTER_SOURCE` 宏注册，由 Driver 的 `BuildPipeline()` 组装进 Pipeline。

### Step 2: 添加 BUILD 规则

在 `src/plugin/features/BUILD` 中添加：

```python
cc_library(
    name = "memory_usage_driver",
    hdrs = ["memory_usage_driver.h"],
    strip_include_prefix = "",
    include_prefix = "features",
    visibility = ["//visibility:public"],
    alwayslink = True,  # 确保 REGISTER_FEATURE 静态初始化器被链接
    deps = [
        ":feature_registry",
        "//src/core:engine",
        "//src/server:sse_handler",
        "//src/plugin/sources:memory_usage",
    ],
)
```

并在 `all_drivers` target 的 deps 中添加 `"//src/plugin/features:memory_usage_driver"`（或直接 `:memory_usage_driver`）。

### Step 3: 验证

```bash
bazel build //src/cli:illuminator
sudo ./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example

# 应能在 Feature 列表中看到 memory_usage
curl http://localhost:9527/api/v2/features | python3 -m json.tool
```

---

## 8. 如何新增一个前端页面

以添加一个 **MemoryPage**（内存监控页面）为例：

### Step 1: 创建数据 Hook

`web/src/hooks/useMemoryData.ts`

```typescript
import { useState, useCallback, useEffect } from 'react'
import { getDataSource } from './useDataSource'
import type { DataBatch, DataSource } from '../services/dataSource'

export function useMemoryUtilization(active: boolean, replaySource?: DataSource) {
  const [data, setData] = useState<MemoryDataPoint[]>([])

  useEffect(() => {
    const source = replaySource ?? getDataSource()
    if (!active) return

    const unsub = source.subscribe('memory_utilization', (batch: DataBatch) => {
      // 解析 batch.data 并更新状态
    })
    return unsub
  }, [active, replaySource])

  return { data }
}
```

### Step 2: 创建页面组件

`web/src/pages/MemoryPage.tsx`

```typescript
import { useMemoryUtilization } from '../hooks/useMemoryData'
import FeatureHealthBadge from '../components/FeatureHealthBadge'

export default function MemoryPage() {
  // Always-On: 无需激活 Feature，数据已在后端持续流动

  const { data } = useMemoryUtilization(true)

  return <div>{/* 图表组件 */}</div>
}
```

### Step 3: 注册路由

在 `web/src/App.tsx` 中添加懒加载路由：
```typescript
const MemoryPage = lazy(() => import('./pages/MemoryPage'))
// 在 routes 数组中添加:
{ path: '/memory', element: <MemoryPage /> }
```

### 关键设计点

- **Hook 使用 `getDataSource().subscribe()`**：通过 DataBus SSE 单连接接收实时数据
- **Always-On 模式**：Tier 1-2 Feature 由 daemon 启动时 `FeatureBus::ProbeAll()` 自动启动，前端不需要 `usePageActivation`
- **Health 监控**：使用 `useFeatureHealth(name)` 检测数据可用性
- **支持 Replay**：hook 接受可选 `replaySource` 参数
- **暂停感知**：从 `useTimeStore` 读取 mode，paused 时取消订阅

---

## 9. 测试体系导航

### 后端测试

```bash
# 全量测试
bazel test //src/...

# 单模块
bazel test //src/core/engine/test:all

# Sanitizer
bazel test //src/... --config=asan   # 内存错误
bazel test //src/... --config=tsan   # 数据竞争

# 详细输出
bazel test //src/core/engine/test:pipeline_integration_test --test_output=all
```

| 模块 | 测试数 | 状态 |
|------|--------|------|
| Core Infra (Status, Config, Arena, Queue, Pool, Timer, Channel, Batch) | 10 | ✅ |
| Pipeline 集成测试 | 1 | ✅ |
| Processors (4) | 4 | ✅ |
| Aggregators (1) | 1 | ✅ |
| Sinks (7) | 7 | ✅ |
| Source 插件 (依赖 eBPF) | 0 | ❌ 需要 BPF mock |

### 前端测试

```bash
cd web

# 运行全部测试 (56 tests)
npm test

# 带覆盖率
npm run test:coverage

# 监视模式
npx vitest --watch
```

| 模块 | 测试数 | 状态 |
|------|--------|------|
| Data Hooks (useCpuData, useGpuData, etc.) | ~30 | ✅ |
| Services (apiClient, dataBus) | ~15 | ✅ |
| Workers (flameGraphWorker) | ~10 | ✅ |
| 组件渲染测试 | 0 | ❌ 待补充 |
| E2E (Playwright) | 0 | ❌ 待补充 |

---

## 10. 常见问题与陷阱

### Q1: 为什么 FeatureDriver 必须设置 `alwayslink = True`？

`REGISTER_FEATURE` 宏生成静态全局变量，在 `main()` 之前自动注册到 FeatureRegistry。没有 `alwayslink`，链接器会丢弃整个编译单元。

### Q2: 为什么 LockFreeQueue 要求容量是 2 的幂？

取模 `pos % capacity` 优化为位掩码 `pos & (capacity - 1)`。构造函数自动向上取整。

### Q3: Push Source 和 Pull Source 该选哪种？

| 维度 | Pull | Push |
|------|------|------|
| 实时性 | 采集间隔内有延迟 | 事件发生即推送 |
| 复杂度 | 简单（实现 `Collect()`） | 复杂（回调+线程安全） |
| 典型场景 | `/proc` 文件、系统指标 | eBPF ring buffer、事件流 |

### Q4: 前端数据 hook 中的 intervalMs 参数有什么用？

目前已是**遗留参数**。hooks 不再自己管理轮询定时器——由 DataBus 通过 SSE 推送数据。该参数保留在签名中以保持向后兼容，但不影响实际行为。

### Q5: SSE 连接断开时前端会卡住吗？

不会。`SseLink` 在连接断开时自动重连，使用指数退避（1s → 30s max）。重连期间 DataBus ringBuffer 中已有数据仍可查看。

### Q6: ProfileSnapshot 如何使用 DataBus？

火焰图通过 `getDataSource().subscribe('cpu_profiler', cb)` 订阅 SSE 推送的 profile 数据，在 hook 内累积 StackSample 后交给 Web Worker 构建火焰树。Tier 3 Feature 需先通过 `/api/v2/features/cpu_profiler/start` 启动。

### Q7: 如何给后端配置认证？

创建 `illuminator.yaml` 并添加：
```yaml
server:
  http:
    listen: "0.0.0.0:9527"
  auth_token: "your-secret-token"
```

前端需要通过 Vite proxy 或直连时在 fetch 请求中携带 `Authorization: Bearer <token>` 头。SSE 长连接在开发模式下通过同源 proxy 访问。

### Q8: 默认配置中为什么没有 auth_token？

开发模式下不强制认证，方便调试。生产部署应通过配置文件设置 `server.auth_token`。

---

## 11. 未来展望与开发路线图

### ✅ 已完成

- HTTP API Bearer Auth 认证
- SSE 实时推送（SseHandler + DataBus，替代 WebSocket）
- RFC v3 架构（InfrastructureManager + FeatureBus + FeatureDriver）
- 火焰图 Web Worker 异步计算
- 死代码清理 + 依赖精简
- 前端 Vitest 测试
- SQLite WAL 存储 + 自动 Prune
- .so 插件动态加载 (SoLoader + 热加载 API)

### 近期（P2）

| 项 | 说明 |
|-----|------|
| ~~Replay 流式解析~~ | ✅ 已完成：`ReadableStream` + 进度回调 + bulk fallback |
| ~~前端组件测试~~ | ✅ 已完成：Vitest + Testing Library，74 项测试 |
| ~~E2E 测试~~ | ✅ 已完成：Playwright 配置 + smoke.spec.ts |
| ~~SSE 数据面~~ | ✅ 已完成：SseHandler 订阅模型 + /api/v1/events/* |
| ~~FeatureBus 控制面~~ | ✅ 已完成：/api/v2/features/* + FeatureDriver 自注册 |
| ~~/pipelines API~~ | ✅ 已完成：新增 `active_features` 字段 |
| mem_tracer 集成 | 将 `mem_tracer.bpf.c` 集成为 `heap_profiler` Source |
| Hook 签名清理 | 移除 hooks 中无实际作用的 `intervalMs` 参数 |

### 中期

| 项 | 说明 |
|-----|------|
| 分布式追踪 | OTLP 完整实现，对接 Jaeger/Tempo |
| 多节点聚合 | 中心化 collector 聚合多个实例 |
| GPU 监控 | NVIDIA GPU 利用率/显存/温度 |
| Dashboard 自定义 | 用户可配置仪表盘布局 |
| Kubernetes Operator | K8s 原生部署和自动发现 |

### 长期

| 项 | 说明 |
|-----|------|
| 告警引擎 | 规则/阈值告警 |
| Grafana 数据源 | 作为 Grafana 插件被引用 |
| WASM 插件 | 多语言插件支持 (Rust/Go) |
| Cgroup 感知 | 容器级资源隔离采集 |

---

## 附录 A: 关键文件速查表

| 你想了解... | 去看... |
|------------|--------|
| 程序入口 | `src/cli/main.cc` |
| 数据长什么样 | `src/core/common/data_batch.h` |
| 管道怎么工作 | `src/core/engine/feature_driver.h`（BuildPipeline） |
| 共享基础设施 | `src/core/engine/infrastructure_manager.h` |
| Feature 注册与生命周期 | `src/core/engine/feature_bus.h` |
| Feature 自动注册 | `src/plugin/features/feature_registry.h` |
| 配置怎么解析 | `src/core/common/yaml_config_loader.h` |
| API 有哪些 | `src/server/api_routes.h` + `/api/v2/features/*` |
| SSE 推送怎么做 | `src/server/sse_handler.h` |
| 如何写 FeatureDriver | `src/plugin/features/cpu_utilization_driver.h` |
| 如何写 Source | `src/plugin/sources/cpu/cpu_utilization/cpu_utilization.h` |
| 如何写 Processor | `src/plugin/processors/filter/filter_processor.h` |
| 如何写 Sink | `src/plugin/sinks/console_output/console_sink.h` |
| 错误怎么处理 | `src/core/common/status.h` |
| 内存怎么管理 | `src/core/memory/arena.h` |
| 线程怎么调度 | `src/core/engine/timer_wheel.h` |
| eBPF 怎么加载 | `src/plugin/sources/ebpf_skeleton_source.h`（skeleton 基类）|
| 前端数据流 | `web/src/services/dataBus.ts` |
| 前端 SSE 传输层 | `web/src/services/sseLink.ts` |
| 前端路由 | `web/src/App.tsx` |
| 前端数据 hook | `web/src/hooks/useCpuData.ts` |
| 火焰图实现 | `web/src/components/charts/ProfileSnapshot.tsx` |
| REST 客户端 | `web/src/services/apiClient.ts` |
| 完整配置参考 | `illuminator.yaml.example` |
| 审计报告 | `docs/code_review_report.md` |
| 架构设计 | `docs/pipeline_v3_design.md` |

---

## 附录 B: 架构层次图

```
┌─────────────────────────────────────────────────────────┐
│              Frontend (React 18 + TypeScript)             │
│  Pages → Hooks → DataBus (SSE via SseLink)              │
│  ProfileSnapshot → Worker → FlameGraph div rendering    │
│  ECharts 6 (tree-shaken) for time-series charts         │
└──────────────────────────┬──────────────────────────────┘
                           │ REST (/api/v2/*) + SSE (/api/v1/events/*)
┌──────────────────────────▼──────────────────────────────┐
│                     CLI (main.cc)                        │
│  daemon | collect | top | version | plugins | storage   │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Server Layer                                │
│  HttpServer (cpp-httplib) + API Routes                   │
│  SseHandler (SSE 订阅管理 + SseSink 推送)                │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Engine Layer (RFC v3)                       │
│  FeatureBus (注册/生命周期) + FeatureDriver (自包含 Pipeline) │
│    ├── InfrastructureManager                            │
│    │     ├── TimerWheel (timerfd+epoll)                 │
│    │     ├── CollectPool (ThreadPool)                   │
│    │     └── SinkPool (ThreadPool)                      │
│    └── FeatureDriver[] (Source → Channel → Sink)        │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Plugin Layer                                │
│  Sources | Processors | Aggregators | Sinks              │
│  PluginRegistry (macro) | FeatureRegistry (REGISTER_FEATURE) │
│  SoLoader (.so) | WASM (stub)                           │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Infrastructure Layer                        │
│  Arena | LockFreeQueue | Status/StatusOr | ConfigValue  │
│  spdlog | InternalMetrics | ResourceLimiter             │
│  StorageBackend (SQLite WAL) | JSON Serializer          │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              eBPF Subsystem                              │
│  libbpf Skeleton | FeatureProbe | StackTraceUtil        │
│  7 probes: cpu_profiler/sampler, offcpu, sched(×2),     │
│            bio_latency, net_tracer                       │
│  vmlinux.h | event_types.h (kernel/userspace shared)    │
└─────────────────────────────────────────────────────────┘
```

---

> **最后建议**：从 `main.cc` 的 `RunDaemon()` 开始，跟踪一次 CPU 采集的完整数据流——从 `InfrastructureManager` 启动、`FeatureRegistry::RegisterAll()`、`FeatureBus::ProbeAll()` 自动启动 Tier 1-2、TimerWheel 调度、CollectPool 执行、AsyncChannel 传输、ProcessThread 处理、到 **SseSink → SseHandler 推送给前端 DataBus**——就能理解整个系统的运转方式。前端是纯数据查看器，打开页面即可通过 `getDataSource().subscribe()` 订阅已在流动的数据。
