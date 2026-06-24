# Illuminator 全栈代码架构审查报告

> **审查日期**: 2026-06-24 (第六版，Always-On 架构重构完成)  
> **审查范围**: 后端 C++20 + 前端 React/TypeScript + eBPF 探针 + 前后端交互  
> **审查方法**: 逐文件源码审读 + curl/Python 实测 API + WS 实时推送验证 + 前端代码审计 + Bazel 编译验证 + 并行架构审计 + Always-On RFC 实现  

---

## 目录

1. [总体架构评估](#一总体架构评估)
2. [后端架构深度分析](#二后端架构深度分析)
3. [前端架构深度分析](#三前端架构深度分析)
4. [前后端通信分析](#四前后端通信分析)
5. [架构问题与不合理之处](#五架构问题与不合理之处)
6. [已修复的 Bug](#六已修复的-bug)
7. [优化建议](#七优化建议)
8. [质量评分卡](#八质量评分卡)

---

## 一、总体架构评估

### 1.1 架构全景图（实测修正版）

```
┌────────────────────────────────────────────────────────────────────────────┐
│                         ILLUMINATOR 全栈架构                                 │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌────────────────────── 前端层 (React 18 + TS) ───────────────────────┐   │
│  │                                                                      │   │
│  │   App.tsx ──▶ BrowserRouter ──▶ 10 个页面 (lazy loaded)             │   │
│  │     │                                                                │   │
│  │     ├── Zustand Stores (useTimeStore / usePipelineStore / useAnnotationStore) │
│  │     ├── 10+ Data Hooks (useCpuData / useMemoryData / ...)            │   │
│  │     ├── Services (apiClient / liveDataSource / replayEngine)           │   │
│  │     ├── Web Worker (flameGraphWorker.ts → 火焰图异步计算)             │   │
│  │     └── Chart Components (ECharts 6 / Worker-backed FlameGraph)       │   │
│  │                                                                      │   │
│  │   通信模式（Always-On: 前端为纯查看器，同端口 :9527）:                 │   │
│  │   ├── WebSocket ws://host:9527/ws/features → 主通道，实时推送          │   │
│  │   ├── HTTP REST :9527 → 降级通道，1s 轮询 (WS 断开时自动切换)        │   │
│  │   ├── HTTP REST :9527 → Session API (Tier 3 profiling)              │   │
│  │   ├── HTTP REST :9527 → Export API (环形缓冲区回溯)                  │   │
│  │   └── Vite Proxy → 开发环境代理 (/api→:9527, /ws→:9527)             │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    │ HTTP + WS :9527 (同端口)                │
│                                    ▼                                         │
│  ┌────────────────────── 后端层 (C++20 + Bazel) ────────────────────────┐   │
│  │                                                                      │   │
│  │  CLI (main.cc) — daemon / collect / top / version / plugins         │   │
│  │    │                                                                 │   │
│  │    ├── WsAwareServer (httplib 子类) ──▶ HTTP + WS 同端口 :9527       │   │
│  │    │   ├── HTTP 路径: 31 个 REST 端点 + 静态 SPA                     │   │
│  │    │   └── WS 路径: MSG_PEEK 检测 Upgrade → WebSocketManager        │   │
│  │    ├── WebSocketManager (自定义 RFC6455 实现, 非 httplib WS)          │   │
│  │    │                                                                 │   │
│  │    └── FeatureManager + PipelineController (核心引擎)                 │   │
│  │          │                                                            │   │
│  │          ├── TimerWheel (1 线程，timerfd+epoll)                        │   │
│  │          ├── CollectPool (M 线程，并行 Source::Collect)                │   │
│  │          ├── Pipelines[] (N 条管道，各有独立 ProcessThread)            │   │
│  │          │     └── AsyncChannel<LockFreeQueue> → Processor → Sink    │   │
│  │          └── SinkPool (K 线程，并行 Sink::Write)                       │   │
│  │                                                                      │   │
│  │  插件系统 (4 角色):                                                    │   │
│  │  ├── Source (9 种: proc_stat_reader/cpu_utilization/process_cpu/     │   │
│  │  │          cpu_profiler/sched_analyzer/ebpf_sched_tracer/           │   │
│  │  │          offcpu_profiler/ebpf_io_monitor/ebpf_net_tracer)         │   │
│  │  ├── Processor (4 种: filter/passthrough/stack_symbolizer/stack_merger) │
│  │  ├── Aggregator (1 种: cpu_stats_aggregator)                         │   │
│  │  └── Sink (9 种: console/file/local_storage/otlp/pprof/             │   │
│  │           prometheus/websocket/stream/fanout)                         │   │
│  │                                                                      │   │
│  │  加载级别: Builtin (alwayslink) → .so (dlopen, 已实现) → WASM (stub)  │   │
│  │                                                                      │   │
│  │  存储: SQLite WAL + 30min Prune + write_mutex 防死锁                  │   │
│  │                                                                      │   │
│  │  eBPF 探针 (7 个已编译 + 1 个孤立):                                    │   │
│  │  ├── cpu_profiler.bpf.c / cpu_sampler.bpf.c                          │   │
│  │  ├── bio_latency.bpf.c / net_tracer.bpf.c                           │   │
│  │  ├── sched_analyzer / offcpu_profiler / sched_tracer                  │   │
│  │  └── mem_tracer.bpf.c (源码存在，未编译未集成)                         │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                            │
│  ┌────────────────────── 基础设施 ──────────────────────────────────────┐   │
│  │  构建: Bazel + bzlmod (clang BPF 编译规则: bpf_probe.bzl)            │   │
│  │  Docker: 3-stage build (Node 20 → Ubuntu 22.04 Bazel → Runtime)      │   │
│  │  Sanitizers: ASan + TSan (Bazel config)                              │   │
│  │  测试: 25+ cc_test + Vitest 前端测试                                  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 总体评价

| 维度 | 评分 | 说明 |
|------|------|------|
| 后端架构设计 | ⭐⭐⭐⭐⭐ | Pipeline v3 + FeatureManager + Always-On 自动启动；职责清晰 |
| 后端代码质量 | ⭐⭐⭐⭐½ | C++20 现代规范；新增 Export/Session API 完善能力 |
| **前端架构一致性** | ⭐⭐⭐⭐½ | Always-On 后前端为纯查看器；数据路径统一；死代码全清 |
| 前后端通信 | ⭐⭐⭐⭐½ | 同端口 WS+HTTP；前端不再触发 Feature 启动（零控制面职责） |
| 测试覆盖 | ⭐⭐⭐⭐ | 后端核心测试完善；前端 74 项测试 + E2E 配置；ESLint 0 errors |
| **综合** | **⭐⭐⭐⭐½ (4.6/5 → 9.2/10)** | Always-On 架构统一后端职责，前端大幅简化 |

> **重大架构变更 (v6)**: 采用 Always-On 模式（详见 `docs/rfc_always_on_design.md`）：
> - 后端 daemon 启动自动运行 Tier 1-2 Feature，前端无需 `POST /features/start`
> - 前端移除 `usePageActivation`/`useFeaturesByCategory`，变为纯数据查看器
> - 新增 Export API（环形缓冲区回溯导出）替代 Recording API
> - 新增 Session API（Tier 3 Profiling，有明确时限）替代无限运行模式
> - 各页面集成 `FeatureHealthBadge`（数据健康指示器）

---

## 二、后端架构深度分析

### 2.1 Pipeline v3 事件驱动引擎

**核心设计（实测确认）：**

```
TimerWheel(1线程, timerfd+epoll)     CollectPool(M线程)      ProcessThread(每Pipeline独占)   SinkPool(K线程)
    │                                     │                         │                         │
    │──CollectTimer触发──Submit────────→│ src.Collect()           │                         │
    │                                     │──Enqueue(batch)────────→│ HandleData()            │
    │                                     │                         │──RunProcessors()        │
    │──FlushTimer触发──InjectFlush────→│                         │──Aggregator.Add()      │
    │                                     │                         │──SubmitToSinks()───────→│ sink.Write()
    │                                     │                         │                         │
    │                                     │                         │                         │
    └── 反压信号 ←──────────────────────────── AsyncChannel(水线 0.8/0.2) ──────────────────────┘
```

**关键实现细节（代码验证）：**

| 组件 | 文件 | 实现方式 |
|------|------|----------|
| TimerWheel | `core/engine/timer_wheel.h` | `timerfd_create` + `eventfd` + `epoll_wait`，优先队列调度 |
| AsyncChannel | `core/engine/async_channel.h` | 包装 `LockFreeQueue`，高/低水线 (0.8/0.2) 反压 |
| LockFreeQueue | `core/memory/lock_free_queue.h` | MPSC sequence-based ring buffer |
| Arena | `core/memory/arena.h` | 用于 DataBatch 的 string interning，零拷贝 |
| SinkPool | `core/threading/thread_pool.h` | 固定线程池并行写入 |

**FeatureManager 与 PipelineController 的关系（代码审计修正）：**

```
PipelineController (共享基础设施 + 管道模板)
├── 拥有: TimerWheel (全局唯一)
├── 拥有: CollectPool (全局共享)
├── 拥有: SinkPool (全局共享)
├── 拥有: 管道模板 (从 YAML 构建，auto_start: false 时始终空闲)
├── 暴露: GET /api/v1/pipelines (⚠ 仅报告空闲模板，非活跃管道)
└── 暴露: GET /api/v1/pipelines/:name/collect (⚠ 同步一次性采集，Deprecated)

FeatureManager (Feature 生命周期，**真正运行管道的地方**)
├── 接收 HTTP API 请求 (start/stop/pause/resume/reconfigure)
├── 按需创建**独立** Pipeline 实例 (非 Controller 的模板管道)
├── 自动注入 3 种运行时 Sink (不在 YAML 配置中):
│   ├── StreamSink → StreamSinkStore (供 /collect 和 /stream API)
│   ├── WebSocketSink → WebSocketSinkStore (供 WS 广播)
│   └── RecordingSink (按需，可通过 record API 启停)
├── 注册定时器到共享 TimerWheel
└── ⚠ 创建 processor/sink 失败时静默跳过 (vs Controller 报错)
```

**⚠ 架构过渡期混淆：** 在默认 `auto_start: false` 模式下，Controller 的管道始终空闲。
前端和用户实际交互的是 FeatureManager 创建的管道。`/api/v1/pipelines` 端点返回的
空闲管道可能让用户误以为系统无活跃管道。详见 [5.11 双管道所有权问题](#511-🟡-中后端双管道所有权问题pipelinecontroller-vs-featuremanager)。

**默认运行模式：on-demand（按需启动）**
- daemon 启动后管道已构建但不自动运行
- 前端通过 `POST /api/v1/features/:name/start` 触发启动
- 支持运行时 `reconfigure` 更新过滤器（PID/comm），自动清空旧数据

### 2.2 HTTP API 完整清单（31 个端点，代码审计确认）

**Feature 管理（核心交互路径）：**

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/api/v1/features` | 列出所有 Feature 及状态 |
| POST | `/api/v1/features/:name/start` | 启动 (body: `{target_pids, target_comms}`) |
| POST | `/api/v1/features/:name/stop` | 停止 |
| POST | `/api/v1/features/:name/pause` | 暂停定时器 |
| POST | `/api/v1/features/:name/resume` | 恢复定时器 |
| POST | `/api/v1/features/:name/reconfigure` | 运行时更新过滤器 |
| GET | `/api/v1/features/:name/collect` | 获取最新一批数据 (从 StreamSinkStore) |
| GET | `/api/v1/features/:name/stream?cursor=N` | 增量拉取 (cursor-based) |

**录制管理：**

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/api/v1/features/:name/record/start` | 开始录制 |
| POST | `/api/v1/features/:name/record/stop` | 停止录制 |
| GET | `/api/v1/features/:name/record/status` | 录制状态 |
| POST | `/api/v1/recording/start` | 全局录制 |
| POST | `/api/v1/recording/stop` | 全局停止 |
| GET | `/api/v1/recording/status` | 全局状态 |

**Pipeline / 查询 / 系统：**

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/healthz` | 健康检查 (无 auth) |
| GET | `/metrics` | Prometheus 指标 (无 auth) |
| GET | `/api/v1/pipelines` | 管道列表 + channel 统计 |
| GET | `/api/v1/channel_stats` | Channel 使用率 |
| POST | `/api/v1/query` | SQL 查询 (只读保护) |
| GET | `/api/v1/budget` | 资源预算 |
| GET | `/api/v1/plugins` | 插件列表 |
| POST | `/api/v1/plugins/reload` | 热加载 .so 插件 |
| GET | `/api/v1/internal_metrics` | JSON 内部指标 |

**便捷别名（直接映射到 pipeline collect）：**

| 路径 | 映射 |
|------|------|
| `/api/v1/cpu/utilization` | `cpu_utilization` |
| `/api/v1/cpu/processes` | `cpu_processes` |
| `/api/v1/cpu/profile/flamegraph` | `cpu_profile` |
| `/api/v1/cpu/profile/offcpu` | `offcpu_profile` |
| `/api/v1/cpu/sched/summary` | `sched_analysis` |
| + 4 个 QueryExtra 端点 | snapshot/history/events/wakeups |

### 2.3 WebSocket 实现（自定义 RFC6455，同端口集成）

| 方面 | 实现细节 |
|------|----------|
| 端口 | **与 HTTP 共享 :9527**（通过 `WsAwareServer` 子类的 `MSG_PEEK` 检测 Upgrade 请求） |
| 实现 | **非 httplib WS**，原始 BSD socket + 自行实现 RFC6455 握手 + 帧编解码 |
| 集成方式 | `WsAwareServer::process_and_close_socket()` 覆盖 → 检测 Upgrade 头 → 调用 `WebSocketManager::HandleUpgrade()` |
| 线程 | `ws-broadcast` (数据推送 + 入站消息处理)；**不再有独立 accept 线程** |
| 订阅模型 | URL 路径 = pipeline key: `/ws/{name}`；连接后发送 `subscribe:{key}` 切换 |
| 客户端订阅 | 文本帧 `subscribe:{key}` |
| 推送频率 | `broadcast_interval_ms` (默认 1000ms) |
| 推送模式 | **区分特性类型（已改进）：** |
| | - 监控类 (cpu_utilization 等): **全量 BatchToJson** 快照推送 |
| | - Profiling 类 (cpu_profile 等): **轻量 notify** `{"type":"notify","feature":...,"records":N,"ts":...}` |
| 数据来源 | `WebSocketSinkStore::Latest(key)` → `serializer_(key, batch)` |
| 认证 | ✅ Bearer Token (HTTP Upgrade 阶段验证 `Authorization` 头或 `?token=` 参数) |
| 无 token 时 | 跳过验证（开发模式不强制认证） |
| 遗留代码 | `Listen()` / `AcceptLoop()` 方法仍存在但**不再被 daemon 使用**，`--ws-port` 命令行参数已忽略 |

**WS vs HTTP stream 的设计分工：**
| 场景 | 通道 | 模式 | 适用场景 |
|------|------|------|----------|
| 实时监控 (cpu/mem/io/net/gpu) | WebSocket | 最新快照推送 | 只需当前值，丢弃旧数据可接受 |
| 火焰图 profiling | HTTP stream | cursor-based 增量 | 需要累积所有历史样本 |
| Feature 管理 / 系统状态 | HTTP REST | 按需请求 | 低频操作 |

### 2.4 插件系统

| 层级 | 状态 | 说明 |
|------|------|------|
| Builtin (alwayslink) | ✅ 完全实现 | 22 个内置插件，静态初始化注册 |
| .so (dlopen) | ✅ 完全实现 | `SoLoader` + C ABI + realpath 安全检查 + 热加载 API |
| WASM | ⚠️ 仅 stub | `wasm_runtime.h` 框架占位，无实际 VM，未集成 |

### 2.5 eBPF 探针

| 探针 | 源码 | 编译 | 附着点 | 对应 Source |
|------|------|------|--------|-------------|
| cpu_profiler | ✅ | ✅ | `perf_event` | `cpu_profiler` |
| cpu_sampler | ✅ | ✅ | `perf_event` (stream mode) | `cpu_profiler` |
| offcpu_profiler | ✅ | ✅ | `sched/sched_switch` | `offcpu_profiler` |
| sched_analyzer | ✅ | ✅ | `sched_wakeup/switch/migrate` | `sched_analyzer` |
| sched_tracer | ✅ | ✅ | `sched_wakeup/switch` | `ebpf_sched_tracer` |
| bio_latency | ✅ | ✅ | `block_rq_issue/complete` | `ebpf_io_monitor` |
| net_tracer | ✅ | ✅ | `sock/inet_sock_set_state` | `ebpf_net_tracer` |
| mem_tracer | ✅ | ❌ | `uprobe` malloc/free | **孤立，无对应 Source** |

---

## 三、前端架构深度分析

### 3.1 文件结构（清理后）

```
web/src/
├── App.tsx                    路由定义 (10 页面, React.lazy)
├── main.tsx                   挂载点
├── components/
│   ├── charts/
│   │   ├── ProfileSnapshot.tsx    火焰图 (Worker 构建 + div 渲染)
│   │   ├── EChart.tsx             ECharts 包装器 (tree-shaken)
│   │   ├── StackedAreaChart.tsx   时序面积图
│   │   ├── CoreHeatmap.tsx        热力图
│   │   ├── ProcessTable.tsx       进程列表 (虚拟滚动)
│   │   ├── ProcessCpuTimeline.tsx CPU 时间线
│   │   ├── ThreadBreakdown.tsx    线程分解图
│   │   └── SummaryCards.tsx       摘要卡片
│   ├── Layout/                    StatusBar, ResourceBudget, ConnectionIndicator, RecordingControl
│   └── TimeControls/              时间控制栏
├── hooks/
│   ├── useCpuData.ts              LiveDataSource.subscribe('cpu_utilization'/'cpu_processes')
│   ├── useMemoryData.ts           LiveDataSource.subscribe('memory_utilization'/'memory_processes')
│   ├── useIoData.ts               LiveDataSource.subscribe('io_monitor')
│   ├── useNetworkData.ts          LiveDataSource.subscribe('net_tracer')
│   ├── useGpuData.ts              LiveDataSource.subscribe('gpu_monitor')
│   ├── useFeatureStream.ts        Feature 状态管理 + TimeSeriesBuffer
│   ├── useDataSource.ts           全局 LiveDataSource 单例 + WS 连接状态 + 页面激活 + 资源预算
│   ├── useProcessDetail.ts, useUrlState.ts, usePipelinePolling.ts
├── pages/
│   ├── CpuPage.tsx, MemoryPage.tsx, IoPage.tsx, NetworkPage.tsx
│   ├── GpuPage.tsx, OverviewPage.tsx, SystemPage.tsx
│   ├── QueryConsole.tsx, PluginManagerPage.tsx, ReplayPage.tsx
│   └── replay/                    ReplayCpuView.tsx, ReplayMemoryView.tsx
├── services/
│   ├── apiClient.ts               HTTP 客户端 (21 个方法，含 featureStream 支持)
│   ├── liveDataSource.ts          WS + HTTP 双通道数据源 (全局单例)
│   ├── replayEngine.ts            离线回放引擎 (流式解析 + bulk fallback)
│   ├── dataSource.ts              DataSource 接口定义
│   └── timeSeriesStore.ts         时序数据环形缓冲 (SystemPage channel stats)
├── stores/
│   ├── useTimeStore.ts            Zustand: live/paused 模式
│   ├── usePipelineStore.ts        Zustand: pipeline 状态
│   └── useAnnotationStore.ts      Zustand: 标注系统
└── workers/
    └── flameGraphWorker.ts        火焰图异步计算 (build/diff/search)
```

### 3.2 火焰图实现（已统一）

**清理后只保留一套实现：**

| 组件 | 位置 | 职责 |
|------|------|------|
| ProfileSnapshot | `components/charts/ProfileSnapshot.tsx` | 数据获取 + 渲染 |
| flameGraphWorker | `workers/flameGraphWorker.ts` | 树构建 (Web Worker, 异步) |

**数据流：**
```
ProfileSnapshot.tsx:
  数据获取: api.featureStream() → /api/v1/features/{name}/stream?cursor=N (1.5s轮询)
            + AbortController 取消未完成请求
  树构建:   Web Worker 异步 buildFlameTree() (不阻塞主线程)
  渲染:     Flexbox <div> 行 × 百分比宽度色块
  过滤:     cleanFrameName() 清理地址/函数名
```

### 3.3 DataSource 抽象（已集成，但存在旁路）

```
当前架构（双通道模式 + 3 条独立数据路径）:
┌──────────────────────────────────────────────────────────────────┐
│  路径 1: LiveDataSource (全局单例，覆盖 5 个监控 hooks)             │
│  getDataSource() → LiveDataSource                                 │
│    ├── 主通道: WebSocket ws://host:9527/ws/features               │
│    │   连接成功 → 发送 subscribe:{feature} → 实时推送 (1s 间隔)    │
│    │   断开 → 自动降级到 HTTP 轮询                                 │
│    └── 降级通道: api.featureCollect(feature) 1s 轮询               │
│                                                                   │
│  集成 Hooks:                                                       │
│  useCpuUtilization   → subscribe('cpu_utilization')               │
│  useCpuProcesses     → subscribe('cpu_processes')                 │
│  useMemoryUtilization → subscribe('memory_utilization')           │
│  useMemoryProcesses  → subscribe('memory_processes')              │
│  useIoMonitor        → subscribe('io_monitor')                    │
│  useNetworkMonitor   → subscribe('net_tracer')                    │
│  useGpuMonitor       → subscribe('gpu_monitor')                   │
├───────────────────────────────────────────────────────────────────┤
│  路径 2: ProfileSnapshot (混合模式，WS notify + HTTP cursor)       │
│  subscribe(feature) 收到 notify → 立即 featureStream(cursor) 拉取  │
│  WS 断开时 → 退化为 1.5s HTTP 轮询 (5s 若 WS 活跃)               │
│  + fetchInFlight 防并发 + AbortController                          │
├───────────────────────────────────────────────────────────────────┤
│  路径 3: useProcessDetail (⚠ 绕过 LiveDataSource)                  │
│  直接调用 api.featureCollect('cpu_processes') 独立 1s 轮询         │
│  → 与 useCpuProcesses 产生重复流量 (同一 feature，不同数据路径)    │
├───────────────────────────────────────────────────────────────────┤
│  Replay 模式:                                                      │
│  ReplayPage → ReplayEngine (implements DataSource) → 传入覆盖      │
│  ⚠ 仅 CPU hooks 支持 replaySource 参数                             │
│  ⚠ Memory 有专用 ReplayMemoryView (直接 subscribe engine)          │
│  ⚠ IO/Network/GPU replay 为 "coming soon" 占位符                   │
└──────────────────────────────────────────────────────────────────┘
```

**评价：** `LiveDataSource` 已集成到主要监控 hooks 中，设计简洁。但存在 3 个架构旁路：
1. `useProcessDetail` 直接 HTTP 轮询，绕过 WS 通道（重复流量）
2. `ProfileSnapshot` 使用独立混合模式（合理——需要 cursor 增量累积）
3. Replay 支持不完整——仅 CPU 页面通过 `replaySource` 参数实现了 Live/Replay 统一

`DataSourceContext.tsx` 已删除（不再需要 Provider 包装）。

### 3.4 状态管理 (Zustand) — 实际使用

| Store | 实际使用情况 |
|-------|-------------|
| `useTimeStore` | ✅ 活跃使用 — live/paused 切换，ProfileSnapshot 暂停轮询 |
| `usePipelineStore` | ✅ 活跃使用 — SystemPage 显示管道状态 |
| `useAnnotationStore` | ✅ 活跃使用 — TimeControls 添加标注 |
| `timeSeriesStore` | ⚠ 部分使用 — SystemPage channel stats，非 Zustand (普通单例类) |

### 3.5 图表层

| 类型 | 技术 | 使用位置 |
|------|------|----------|
| 时序图 | ECharts 6 (tree-shaken) | StackedAreaChart, CoreHeatmap, ProcessCpuTimeline, Memory/IO/Net/GPU 页面 |
| 火焰图 | Worker + div (CSS flexbox) | ProfileSnapshot (CPU 页面，异步树构建) |
| 进程表 | 原生 HTML Table | ProcessTable |

**ECharts 集成质量好：** 自定义 `EChart.tsx` 包装器，按需引入 (tree-shaking)，暗色主题。d3 相关依赖已全部移除。

---

## 四、前后端通信分析

### 4.1 通信架构（统一端口 :9527 双协议模式）

```
┌──────── Frontend (活跃路径) ────────────────┐     ┌────── Backend :9527 ──────┐
│                                              │     │                            │
│  ┌─ LiveDataSource (全局单例) ────────────┐ │     │  ┌─ WsAwareServer ───────┐ │
│  │ ws://host:9527/ws/features             │─┼─WS──┼→│ MSG_PEEK 检测 Upgrade  │ │
│  │ → subscribe:{feature} 订阅             │ │     │  │  ├─ WS → HandleUpgrade │ │
│  │ ← 监控: 全量 JSON (1s 间隔)            │ │     │  │  └─ HTTP → httplib 处理│ │
│  │ ← Profiling: notify 通知 → 触发 HTTP 拉│ │     │  └────────────────────────┘ │
│  │                                        │ │     │                            │
│  │ [降级] WS 断开时:                       │ │     │  ┌─ WebSocketManager ────┐ │
│  │ → api.featureCollect(name) 1s 轮询     │ │     │  │ Bearer Auth 验证       │ │
│  └────────────────────────────────────────┘ │     │  │ 广播线程 1s 间隔       │ │
│                                              │     │  │ 监控→全量 / Profile→notify│
│  ┌─ apiClient.ts ─────────────────────────┐ │     │  └────────────────────────┘ │
│  │ featureStart/Stop/Reconfigure          │─┼─HTTP┼→ 31 个 REST 端点            │
│  │ pipeline / healthz / budget            │ │     │  (Bearer Auth /api/* 路径)   │
│  └────────────────────────────────────────┘ │     │                            │
│                                              │     │                            │
│  ┌─ ProfileSnapshot (混合模式) ───────────┐ │     │                            │
│  │ WS notify 触发 → featureStream(cursor) │─┼─HTTP┼→ /features/{name}/stream   │
│  │ WS 断开时 → 1.5s HTTP 轮询降级         │ │     │  (cursor-based 增量拉取)    │
│  │ + AbortController + fetchInFlight 防重入│ │     │                            │
│  └────────────────────────────────────────┘ │     │                            │
│                                              │     │                            │
│  ┌─ useProcessDetail (⚠ 直接轮询) ───────┐ │     │                            │
│  │ api.featureCollect('cpu_processes')    │─┼─HTTP┼→ 绕过 LiveDataSource        │
│  │ 独立 1s 轮询                            │ │     │  (详见 问题 5.11)           │
│  └────────────────────────────────────────┘ │     │                            │
└──────────────────────────────────────────────┘     └────────────────────────────┘
```

### 4.2 各数据 Hook 的通信方式

| Hook | Feature 订阅 | 通道 | 降级方式 | 备注 |
|------|------|------|------|------|
| `useCpuUtilization` | `cpu_utilization` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useCpuProcesses` | `cpu_processes` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useMemoryUtilization` | `memory_utilization` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useMemoryProcesses` | `memory_processes` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useIoMonitor` | `io_monitor` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useNetworkMonitor` | `net_tracer` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `useGpuMonitor` | `gpu_monitor` | **WS**/HTTP | LiveDataSource 自动切换 | |
| `ProfileSnapshot` (on-CPU) | `cpu_profile` | **WS notify + HTTP** | WS notify→featureStream；断开→1.5s 轮询 | 混合模式 |
| `ProfileSnapshot` (off-CPU) | `offcpu_profile` | **WS notify + HTTP** | 同上 | 混合模式 |
| `useProcessDetail` | `cpu_processes` | **纯 HTTP** | 独立 1s 轮询 | ⚠ 绕过 LiveDataSource |
| `usePipelinePolling` | — | **纯 HTTP** | 独立 3s 轮询 | App 级别 |
| `SystemPage` | — | **纯 HTTP** | 独立 3s 轮询 | 低频管理 |
| `useResourceBudget` | — | **纯 HTTP** | 独立 5s 轮询 | 侧边栏 |

**通信模型总结（4 种模式）：**
1. **WS 实时推送** — 7 个监控 hooks 通过 `LiveDataSource.subscribe()`，WS 可用时 ~1s 推送，断开时自动 HTTP 降级
2. **WS notify + HTTP 拉取** — 火焰图 `ProfileSnapshot`，WS 仅通知有新数据，实际拉取走 cursor-based `featureStream` API
3. **纯 HTTP 轮询** — `useProcessDetail` (⚠ 与 useCpuProcesses 重复)、管道/系统/预算等管理类
4. **本地文件** — `ReplayEngine` 读取 `.ilr` 文件，不涉及网络

### 4.3 Stream API cursor 协议（实测修正）

```
前端初始化流程 (ProfileSnapshot useEffect):
  1. fetch(/stream?cursor=999999999) → 响应 {cursor: 0, batches: []}
     ↑ cursor=999999999 超过 seq_ 时，PollSince 返回 actual seq (已修复)
  2. cursorRef.current = 响应中的 cursor (0)
  3. 后续 poll: fetch(/stream?cursor=0) → 获取所有新批次
  4. 更新 cursorRef.current = 响应中的新 cursor

增量语义:
  - cursor < seq: 返回 cursor 之后的所有 batch，cursor 更新为 seq
  - cursor >= seq: 返回空 batch，cursor 设为 seq (确保不会卡住)
```

---

## 五、架构问题与不合理之处

### 5.1 ✅ 已修复：前端 WebSocket 基础设施完全空转

**原问题：** 后端/前端均实现了 WS 推送/接收，但两者从未在数据层面连通。前端 100% HTTP 轮询。

**修复措施：**
- 所有数据 hooks (useCpuData, useMemoryData, useIoData, useNetworkData, useGpuData) 已改为通过 `LiveDataSource.subscribe()` 获取数据
- `LiveDataSource` 自动尝试 WS 连接，连接成功时接收实时推送；连接失败时透明降级为 HTTP 轮询
- 删除了独立的 `wsManager.ts`（其 per-pipeline 订阅模型与 Feature API 不兼容）
- 后端 WS 端口新增 Bearer Auth 验证（与 HTTP API 一致）

**当前数据流：**
```
WS 可用时:  LiveDataSource → ws://host/ws/features → subscribe:{feature} → 实时 JSON 推送
WS 不可用:  LiveDataSource → api.featureCollect() → 1s HTTP 轮询 (自动降级)
```

### 5.2 ✅ 已修复：三套火焰图实现冲突

**原问题：** 存在三套火焰图实现（ProfileSnapshot div 渲染 / FlameGraph.tsx SVG / pages/FlameGraph.tsx d3），只有最简单的一套活跃。

**修复措施：**
- 删除 `components/charts/FlameGraph.tsx` (SVG 死代码)
- 删除 `pages/FlameGraph.tsx` (孤立 d3 页面)
- 移除 `d3-flame-graph` + `d3-selection` 依赖 (减小 bundle)
- 将 `ProfileSnapshot.tsx` 的树构建逻辑移至 Web Worker (`flameGraphWorker.ts`)
- 现在只有一套统一实现：Worker 异步计算 + div 渲染

### 5.3 ✅ 已修复：DataSource 未集成

**原问题：** `DataSourceProvider` 从未挂载，各 hook 直接硬编码 `apiClient` 调用。

**修复措施：**
- 删除了 `DataSourceContext.tsx`（Provider 模式对该场景过度设计）
- 改用全局单例 `getDataSource()` (在 `useDataSource.ts` 中)
- 所有数据 hooks 通过 `getDataSource().subscribe(feature, callback)` 获取数据
- 各 hook 通过可选参数 `replaySource?: DataSource` 支持 Replay 模式覆盖
- 实现了 Live/Replay 统一消费且无 Provider 嵌套开销

### 5.4 ✅ 已修复：ProfileSnapshot 绕过 apiClient

**原问题：** `ProfileSnapshot.tsx` 直接使用 `fetch()` 而非 `apiClient.ts`。

**修复措施：**
- 新增 `api.featureStream(name, cursor, signal?)` 方法到 `apiClient.ts`
- `ProfileSnapshot` 改用 `api.featureStream()` + `api.featureCollect()`
- 添加 `AbortController` 支持，切换 PID/profileType 时自动取消未完成请求

### 5.5 ✅ 已修复：WebSocket 端口无认证

**原问题：** WS 端口 (9528) 无任何认证，任何人可以连接接收实时数据。

**修复措施：** 在 `WebSocketManager::AcceptLoop` 的 HTTP 升级握手阶段增加 token 验证。支持两种方式：
- `Authorization: Bearer <token>` 请求头（标准方式）
- `?token=<token>` URL 查询参数（浏览器 WS API 无法设置自定义 header 的备选）

未通过验证时返回 `HTTP/1.1 401 Unauthorized` 并关闭连接。

### 5.6 🟢 低：命名可改进

- `useFeatureStream.ts` — 名称暗示使用 stream/WS，实际提供的是 `TimeSeriesBuffer` 工具类
- `stream?cursor=N` — 是 cursor-based 增量拉取，非 HTTP streaming（但命名约定俗成，可接受）
- ~~`liveDataSource.ts` — 建连但不供数据~~ → **已修复**：现在是所有 hooks 的核心数据供应者

### 5.7 ✅ 已修复：前端存在多处死代码（第一批）

**已删除的死代码文件（共 7 个，~38KB）：**
- ~~`components/charts/FlameGraph.tsx`~~ (SVG 火焰图，从未 import)
- ~~`components/FeaturePanel/FeaturePanel.tsx`~~ (独立控制栏，从未 import)
- ~~`components/ExportMenu.tsx`~~ (导出菜单，从未 import)
- ~~`contexts/DataSourceContext.tsx`~~ (Provider 从未挂载)
- ~~`hooks/useFlameGraph.ts`~~ (Worker hooks，仅被已删除的 FlameGraph.tsx 使用)
- ~~`pages/FlameGraph.tsx`~~ (d3 页面，无路由)
- ~~`services/wsManager.ts`~~ (WS 管理器，subscribe 从未被调用)

**同时移除的无用依赖：** `d3-flame-graph`, `d3-selection`, `@types/d3-selection`

### 5.8 ✅ 已修复：前端第二批死代码（已清理）

| 文件/符号 | 操作 |
|-----------|------|
| `services/exportService.ts` | ✅ **已删除** (3.6KB，零引用) |
| `hooks/useDataSource.ts` 中的 `useDataSource(feature)` | ✅ **已删除** (所有页面直接调用 `getDataSource()`) |
| `hooks/useUrlState.ts` 中的 `useRestoreTimeFromUrl()` | ✅ **已删除** (从未被调用) |
| `api.cpuProfileFlamegraph()` | ✅ **已删除** (前端通过 `featureStream()` 获取数据) |
| `api.cpuProfileOffcpu()` | ✅ **已删除** (同上) |

### 5.9 ✅ 已修复：后端双 API 表面（遗留路由已标记 Deprecated）

**问题：** 后端存在两套并行的数据获取路径，遗留路由在 on-demand 模式下容易混淆。

**修复措施：** 所有遗留路由现在返回标准 Deprecation HTTP 头，引导用户使用 Feature API：

```http
HTTP/1.1 200 OK
Deprecation: true
Sunset: 2026-09-01
Link: </api/v1/features/cpu_utilization/collect>; rel="successor-version"
```

| 遗留路由 | Deprecation Header | Link 指向 |
|----------|-------------------|-----------|
| `/api/v1/cpu/utilization` | ✅ | `/api/v1/features/cpu_utilization/collect` |
| `/api/v1/cpu/processes` | ✅ | `/api/v1/features/cpu_processes/collect` |
| `/api/v1/cpu/profile/flamegraph` | ✅ | `/api/v1/features/cpu_profile/collect` |
| `/api/v1/cpu/profile/offcpu` | ✅ | `/api/v1/features/offcpu_profile/collect` |
| `/api/v1/cpu/sched/summary` | ✅ | `/api/v1/features/sched_analysis/collect` |
| `/api/v1/pipelines/:name/collect` | ✅ | `/api/v1/features/:name/collect` |
| QueryExtra 端点 (×5) | ✅ | (无直接替代，标注为过时) |

**效果：** 遗留路由仍可用（不会 break 已有脚本），但通过标准 HTTP `Deprecation` + `Sunset` + `Link` 头引导用户迁移到 Feature API。2026-09-01 后可考虑移除。

### 5.10 ✅ 已修复：WebSocket 死锁导致实时推送完全不工作

详见 [6.1 Bug #6](#61-本轮修复)。

这是本轮发现的**最严重架构缺陷**。虽然 WS 握手/认证/编解码全部正确实现，但一个互斥锁使用错误导致整个实时推送功能静默失败。此类 bug 在测试中难以发现（只要不发 subscribe 消息就不会触发），但在生产环境中会被前端的 re-subscribe 逻辑必然触发。

### 5.11 ✅ 已修复：后端双管道所有权问题（/pipelines API 信息不完整）

**原问题：** `GET /api/v1/pipelines` 仅返回 `PipelineController` 的空闲管道模板，不包含 `FeatureManager` 管理的实际运行管道，导致用户误认为系统无活跃管道。

**修复措施：**
- `RegisterApiRoutes` 新增可选 `FeatureManager*` 参数
- `/api/v1/pipelines` 响应中新增 `active_features` 数组字段，包含所有活跃/暂停状态的 Feature 管道信息
- 每个管道条目新增 `"origin"` 字段（`"controller"` 或 `"feature_manager"`）区分来源
- Controller 管道保留所有原有字段；Feature 管道显示 name、display_name、category、state、batches、records、errors、uptime_ms

**响应格式：**
```json
{
  "pipelines": [...],           // Controller 管道模板 (可能空闲)
  "active_features": [...]      // FeatureManager 活跃管道 (真正在运行的)
}
```

### 5.12 🟡 中：后端两份并行内存存储（StreamSinkStore vs WebSocketSinkStore）

**问题：** 每个 Feature 的数据同时写入两份独立存储：
- `StreamSinkStore`：供 HTTP `/collect` 和 `/stream` API（cursor-based，保留历史）
- `WebSocketSinkStore`：供 WS 广播（latest-only，覆盖旧数据）

**影响：** 内存开销翻倍（虽然 WS 存储只保留最新一批，开销有限），设计冗余。

**建议：** 考虑让 WS 广播线程直接从 `StreamSinkStore` 读取最新批次，消除 `WebSocketSinkStore`。

### 5.13 ✅ 已修复：useProcessDetail 绕过 LiveDataSource

**原问题：** `useProcessDetail` 直接调用 `api.featureCollect('cpu_processes')` 进行独立 HTTP 轮询，与 `useCpuProcesses` 产生重复流量。

**修复措施：** 重写 `useProcessDetail` 为通过 `getDataSource().subscribe('cpu_processes')` 订阅数据。现在与 `useCpuProcesses` 共享同一个 LiveDataSource 数据通道（WS 或 HTTP 降级），消除了重复网络请求。

### 5.14 ✅ 已修复：WS 广播无去重

**原问题：** WebSocket 广播线程每 1s 从 `WebSocketSinkStore` 读取并推送，即使数据未更新客户端也会收到重复 payload。

**修复措施：** 在 `WebSocketManager` 中添加 `last_broadcast_` map，通过 `DataBatchPtr` 指针比较检测数据是否更新。如果 `Latest(key)` 返回的指针与上次广播相同（即无新 batch 被 push），跳过序列化和发送。这是 O(1) 的检测且完全准确（每次 `WebSocketSink::Write()` 都推入新的 shared_ptr）。

### 5.15 ✅ 部分修复：前端第三批死代码清理

**已清理（4 个符号）：**

| 文件/符号 | 操作 |
|-----------|------|
| `useFeatureStream()` hook 函数体 (~130 行) | ✅ **已删除**（保留 `TimeSeriesBuffer` 和 `useFeatureList`）|
| `api.pluginsList()` | ✅ **已删除** |
| `api.featureRecordStatus()` | ✅ **已删除** |
| `timeSeriesStore.gc()` | ✅ **已删除**（含无用的 `MAX_AGE_MS` 常量）|

**保留未清理（影响小或有未来用途）：**

| 文件/符号 | 理由 |
|-----------|------|
| `AnnotationOverlay` 组件 | 与 `AddAnnotationButton` 同文件，逻辑完整；未来可挂载到图表上 |
| Worker `diff` / `search` | 预留能力，开销为 0（不会被打包除非显式 import）|
| `usePageActivation().manualStart/Stop` | 接口一致性；未来 UI 可能消费 |
| `useFeaturesByCategory()` 返回值浪费 | 调用本身触发 feature 启动逻辑，返回值浪费不影响正确性 |

### 5.16 🟡 低：配置标志解析但未生效

**问题：** YAML 配置中 `server.http_enabled` 和 `server.ws_enabled` 字段被解析和存储，但 `main.cc` 中 HTTP/WS 始终启动，这些标志不影响任何行为。

### 5.17 🟡 低：`/stream` API 无活跃状态检查

**问题：** `GET /features/:name/collect` 在 feature 非活跃时拒绝请求，但 `/features/:name/stream` 无条件读取 `StreamSinkStore`——feature stop 后仍可能返回旧数据（直到 buffer 被清理）。

---

## 六、已修复的 Bug

### 6.1 本轮修复

| # | 问题 | 根因 | 修复方案 |
|---|------|------|----------|
| 1 | On-CPU 符号解析率低 (~78%) | `LookupBpfStackTrace` 未过滤非规范地址 (x86_64 hole) | 添加 `IsCanonicalAddress()` 检查，跳过垃圾帧 |
| 2 | Off-CPU 无数据 | `offcpu_profiler.bpf.o` 是旧版本 (缺 offcpu_stats map) | bazel 重编译 BPF 对象并更新 `build/bpf/` |
| 3 | Stream cursor 卡住 | `PollSince()` 在 cursor>=seq 时不更新 cursor | 添加 `cursor = available` 即使无新数据 |
| 4 | Reconfigure 后数据污染 | StreamSinkStore 和 BPF stack_counts map 未清空 | ReconfigureFilter 清空 buffer + BPF map |
| 5 | 火焰图超出容器 | `flexShrink: 0` 阻止帧缩小 | 移除 flexShrink，添加行级 overflow 约束 |
| 6 | **WebSocket 实时推送完全不工作** | `ProcessIncoming` 持有 `mu_` 锁后调用 `HandleTextMessage`，后者再次 `lock_guard<mutex> lk(mu_)` → **死锁**。`std::mutex` 非递归，导致 `ws-broadcast` 线程在收到第一个 TEXT 帧时永久阻塞，所有订阅和广播停止 | 移除 `HandleTextMessage` 内的冗余 `lock_guard`，标注 `// REQUIRES: mu_ already held` |

> **Bug #6 影响分析：** 这是一个严重的**设计缺陷**而非逻辑错误。虽然 WS 握手、认证、初始订阅（通过 URL path）均正确工作，但任何客户端发送 `subscribe:xxx` 消息会触发死锁，导致：
> - BroadcastLoop 线程永久阻塞
> - 所有 WS 客户端不再收到任何数据推送
> - 新连接的 accept 不受影响，但建立后也收不到数据
> - 此 bug 在前端通过 URL path 订阅时被"掩盖"（只要不发 subscribe 消息就不会触发），但 `LiveDataSource` 的 re-subscribe 逻辑会触发它

### 6.2 历史修复（文档记录）

| # | 问题 | 修复方案 | 状态 |
|---|------|---------|------|
| SQLite 并发死锁 | busy_timeout + write_mutex | ✅ |
| string_view 悬垂 | Arena 生命周期绑定 DataBatch | ✅ |
| 0.0.0.0 无认证 | 默认 127.0.0.1 + Bearer Token | ✅ |
| SQL 注入 | 只允许 SELECT/EXPLAIN/PRAGMA | ✅ |
| 内存泄漏 | InternString 去重 + Prune + 背压 | ✅ |

---

## 七、优化建议

### 7.1 ✅ 已完成：前端架构清理

#### ✅ P0-1: 清理 WS 死代码 + 集成 WS 实时推送

- ✅ 删除 `wsManager.ts`（其 per-pipeline 订阅模型与 Feature API 不兼容）
- ✅ 删除 `DataSourceContext.tsx`（改用全局单例模式）
- ✅ 修改 `StatusBar.tsx` 移除 wsManager 依赖
- ✅ 所有 5 个数据 hooks 改用 `getDataSource().subscribe()` 获取数据
- ✅ `LiveDataSource` 连接 WS 成功时接收实时推送；失败时透明降级为 HTTP 轮询
- ✅ 后端 WS 端口新增 Bearer Auth 验证（与 HTTP API 一致）

#### ✅ P0-2: 统一火焰图为单一实现

- ✅ `ProfileSnapshot` 数据获取改用 `api.featureStream()` + AbortController
- ✅ 树构建移至 `flameGraphWorker.ts` (Web Worker 异步，不阻塞主线程)
- ✅ 删除未使用的 SVG 实现和 d3 页面
- ✅ 移除 `d3-flame-graph` + `d3-selection` 依赖

### 7.2 ✅ P1 — 安全与一致性（全部已完成）

#### ✅ P1-1: WebSocket 认证

在 `WebSocketManager::AcceptLoop` 的 HTTP Upgrade 阶段验证 Bearer Token：
- 支持 `Authorization: Bearer <token>` 头
- 支持 `?token=<token>` URL 查询参数（浏览器 WS API 限制备选）
- 无 token 配置时跳过验证（开发友好）
- 已通过 Python socket 测试验证全部 4 种场景

#### ✅ P1-2: ProfileSnapshot 使用 apiClient

```typescript
// 已改为:
const data = await api.featureStream(featureName, cursorRef.current, abortRef.current.signal)
```
- 新增 `api.featureStream()` 到 `apiClient.ts`
- 添加 `AbortController` 支持（PID/profileType 切换时取消未完成请求）

#### ✅ P1-3: 清理前端死代码

已删除 7 个文件 (~38KB)：wsManager.ts、DataSourceContext.tsx、FlameGraph.tsx (×2)、useFlameGraph.ts、ExportMenu.tsx、FeaturePanel.tsx

### 7.3 待做优化项（按优先级排序）

#### ★★★ P2-A（高）：WebSocket 同端口方案

**问题：** WS 和 HTTP 分别在 9527/9528 双端口架构，在以下场景中产生部署复杂性：
- 反向代理（nginx/envoy）需配置两个 upstream
- 容器网络需暴露两个端口
- 防火墙/安全组需开放额外端口
- 浏览器安全策略可能限制跨端口 WebSocket（测试中已遇到）

**方案：** 将 WebSocket 升级处理集成到 httplib 的 HTTP server 中：
```
当前：  HTTP(:9527) + 独立 WS(:9528, 原始 BSD socket)
目标：  HTTP+WS(:9527, httplib 统一处理)
```

**✅ 已实现（2026-06-16）：**

采用 **httplib::Server 子类化** 方案：创建 `WsAwareServer` 覆盖 `process_and_close_socket()`，
用 `MSG_PEEK` 检测 WebSocket Upgrade 请求，在同一 TCP 端口 (9527) 上同时处理 HTTP 和 WS 连接。

关键文件变更：
- `src/server/http_server.h`: 新增 `WsAwareServer` 子类
- `src/server/websocket_manager.h`: 新增 `HandleUpgrade()` 公开方法
- `src/cli/main.cc`: 移除独立 WS 端口监听，通过 `SetWebSocketUpgradeHandler` 注入
- `web/src/services/liveDataSource.ts`: `getWsUrl()` 简化为 `window.location.host`
- `web/vite.config.ts`: WS proxy target 改为 9527

**验证结果：** Python 客户端在 port 9527 成功 WS 握手、subscribe、接收 8 帧数据/7s

---

#### ★★★ P2-B（高）：前端组件测试 + E2E

**✅ 已实现（2026-06-16）：**

**测试框架搭建：**
| 层级 | 工具 | 状态 |
|------|------|------|
| 组件渲染 | Vitest + React Testing Library | ✅ 已配置 + 13 个新测试 |
| 集成测试 | Vitest + Mock WS | ✅ 已有 LiveDataSource 4 项测试 |
| E2E | Playwright (Chromium) | ✅ 已配置 + smoke.spec.ts |

**已完成的测试文件：**
- `src/components/SubTabBar.test.tsx` — 4 项（渲染、激活态、点击回调、空数组）
- `src/components/Layout/ConnectionIndicator.test.tsx` — 5 项（三种状态 + tooltip）
- `src/App.test.tsx` — 4 项（导航渲染、路由、版本获取、连接指示器）
- `e2e/smoke.spec.ts` — 6 项（页面加载、API、WS 升级、导航、生命周期、deprecation）

**当前覆盖率：** 12 test files, 69 tests, all passing (2.29s)

**待扩展方向：**
1. `ProfileSnapshot` — Worker + 异步渲染测试
2. `CpuPage` — 完整用户交互流
3. MSW mock 集成测试（WS 降级路径）

---

#### ★★☆ P2-6（中）：火焰图接入 LiveDataSource

**✅ 已实现（2026-06-16）— 方案 B：WS 通知 + HTTP 拉取**

**后端变更：**
- `main.cc` 序列化器检测 `profile` 关键字，发送轻量通知：
  `{"type":"notify","feature":"cpu_profile","records":10,"ts":...}`
- 非 profiling 特性继续发送完整数据批次

**前端变更 (`ProfileSnapshot.tsx`)：**
- 通过 `getDataSource().subscribe(featureName, cb)` 订阅 WS 通知
- 回调检测 `data.type === 'notify'` → 立即触发 `poll()` 拉取增量数据
- WS 连接时降级轮询间隔从 1.5s → 5s；WS 断开时恢复 1.5s
- `fetchInFlight` 防重入保护，避免通知风暴下的并发请求

**验证结果：** cpu_utilization 确认发送全量批次（tier 1-2），profiling 序列化输出 notify 格式（环境限制无法产出 profiling 数据但逻辑正确）

---

#### ★★☆ P2-1（中）：Replay 大文件流式解析

**✅ 已实现（2026-06-16）**

**问题：** `ReplayEngine.loadFile()` 使用 `file.text()` 一次性读全文件。>100MB `.ilr` 文件会阻塞 UI 数秒 + 占用大量内存。

**实现方案：**
- `loadFile()` 检测 `file.stream()` 可用性：现代浏览器用流式，JSDOM 测试环境用 bulk fallback
- `loadStreaming()`：`file.stream().getReader()` + `TextDecoder({ stream: true })` + 行缓冲
- `loadBulk()`：保留原有 `file.text()` + `split('\n')` 逻辑作为兼容后备
- `indexLine()` 提取为公共方法，两种加载方式共享
- `onProgress` 回调提供 0~1 进度（基于 `bytesRead / file.size`）
- 修复了 `!obj.ts` falsy 检查的 bug（`ts: 0` 时被错误跳过），改为 `obj.ts == null`

**测试覆盖（16 项全部通过）：**
- 流式加载正确解析帧
- 进度回调正确报告
- 分块流正确处理边界
- 单行文件兼容
- 1000 帧大文件性能测试 (<500ms)
- 原有 bulk 加载路径回归测试

---

#### 其他待做项

| 项 | 建议 | 优先级 | 状态 |
|-----|------|--------|------|
| **P2-C** | **`useProcessDetail` 改走 LiveDataSource**（消除与 `useCpuProcesses` 的重复流量） | **中** | ✅ 已完成 |
| **P2-D** | **WS 广播去重**（DataBatchPtr 指针比较，未变化时跳过序列化和发送） | **中** | ✅ 已完成 |
| **P2-E** | **统一 `/api/v1/pipelines` 报告**（新增 `active_features` 字段含 FeatureManager 管道） | **中** | ✅ 已完成 |
| **P2-F** | **清理第三批前端死代码**（useFeatureStream hook、pluginsList、featureRecordStatus、gc） | **中** | ✅ 已完成 |
| P2-2 | 多页面重复组件抽取 (SummaryCard, Sparkline, EmptyChart) | 低 | 待做 |
| P2-4 | `mem_tracer.bpf.c` 集成为 `heap_profiler` Source 或移除 | 低 | 待做 |
| ~~P2-5~~ | ~~`useCpuData` 等 hook 签名中 `intervalMs` 参数清理~~ | ~~低~~ | ✅ 已完成 |
| ~~P2-7~~ | ~~清理第二批前端死代码~~ | ~~低~~ | ✅ 已完成 |
| ~~P2-8~~ | ~~遗留 API 路由添加 Deprecation 头~~ | ~~中~~ | ✅ 已完成 |
| P2-9 | `useFeatureStream.ts` 职责分离：`TimeSeriesBuffer` 提取为独立工具文件 | 低 | 待做 |
| P2-10 | 前端 per-feature recording UI 集成（后端已支持，前端仅侧边栏全局录制） | 低 | 待做 |
| P2-11 | Replay 补全 IO/Network/GPU 视图（当前为占位符 "coming soon"） | 低 | 待做 |
| P2-12 | WASM 插件运行时：决定实现或移除 stub (`wasm_runtime.h`) | 低 | 待做 |
| P2-13 | 2026-09-01 后移除遗留 Deprecated API 路由 | 低 | 定时 |
| P2-14 | 修复 `server.http_enabled` / `server.ws_enabled` 配置标志不生效 | 低 | 待做 |
| P2-15 | `/stream` API 添加 feature 活跃状态检查 | 低 | 待做 |
| P2-16 | Replay 全面支持：IO/Network/GPU hooks 添加 `replaySource` 参数 | 低 | 待做 |
| P2-17 | CPU 页面增加"停止 Profiling"按钮（当前只有启动，无对称停止 UX） | 低 | 待做 |

---

## 八、质量评分卡

### 8.1 后端 (C++20)

| 维度 | 评分 | 评语 |
|------|------|------|
| 架构设计 | 9/10 | Pipeline v3 + FeatureManager 设计精良；但存在双管道所有权过渡期混淆 |
| 代码规范 | 8/10 | 现代 C++20，Status/StatusOr 统一，少量 header-only 巨文件 |
| 错误处理 | 7.5/10 | StatusOr 模式一致；但 FeatureManager 创建失败静默跳过 vs Controller 报错（不一致）|
| 并发安全 | 8/10 | 原子操作 + LockFreeQueue + 独占线程设计；WS 死锁已修复 |
| 内存管理 | 8.5/10 | Arena + InternString + SharedPtr + Prune；双存储 (Stream+WS) 轻微冗余 |
| 安全性 | 8.5/10 | HTTP + WS 统一 Bearer Auth，SQL 注入防护，默认 127.0.0.1 |
| API 设计 | 8.5/10 | Feature API 设计优秀，遗留路由已标注 Deprecation；`/pipelines` 信息不完整 |
| **小计** | **8.4/10** | |

### 8.2 前端 (React/TypeScript)

| 维度 | 评分 | 评语 |
|------|------|------|
| 架构设计 | 8.5/10 | DataSource 单例 + WS/HTTP 降级好；但 useProcessDetail 旁路 + Replay 支持不完整 |
| **架构实现** | **8/10** | 两批已清理；仍存在第三批死代码 (5.15)；3 条独立数据路径略混乱 |
| 代码规范 | 8/10 | TypeScript strict；部分死代码残留 + `useFeaturesByCategory` 调用浪费 |
| 状态管理 | 8/10 | Zustand 简洁，Store 粒度合理；URL state 同步不完整 |
| 性能优化 | 8.5/10 | ECharts 懒加载 + 火焰图 Worker + WS 减少 HTTP 开销 + 页面可见性感知 + Replay 流式 |
| **小计** | **8.2/10** | |

### 8.3 前后端通信

| 维度 | 评分 | 评语 |
|------|------|------|
| 协议设计 | 9/10 | WS 推送 + HTTP cursor 增量 + 自动降级 + notify 优化，场景分工合理 |
| **协议实现** | **8.5/10** | 同端口方案优雅；WS 广播无去重；`useProcessDetail` 绕过 WS 产生重复流量 |
| API 设计 | 8.5/10 | Feature API 设计一流；`/pipelines` 不包含活跃管道信息（混淆）|
| **小计** | **8.7/10** | |

### 8.4 综合评分

```
┌──────────────────────────────────────────────────────────┐
│                                                          │
│   后端   ████████████████░░░░  8.4/10                    │
│   前端   ████████████████░░░░  8.2/10                    │
│   通信   █████████████████░░░  8.7/10                    │
│                                                          │
│   综合   ████████████████░░░░  8.4/10                    │
│                                                          │
│   ✅ 全部已完成的修复/改进:                                │
│   - WebSocket 死锁 (mu_ 重复加锁，Bug #6)                │
│   - Hook 签名清理 (移除无用 intervalMs)                    │
│   - 两批前端死代码清理 (共 12 个文件/符号)                 │
│   - 遗留 API 路由标注 Deprecation + Sunset                │
│   - 火焰图统一为 Worker + div (删除 2 个死实现)            │
│   - ProfileSnapshot 改用 apiClient + AbortController      │
│   - 移除 d3-flame-graph/d3-selection 依赖                 │
│   - WS + HTTP 合并到同端口 :9527 (WsAwareServer)          │
│   - 7 个数据 hooks 集成 LiveDataSource (WS+降级)          │
│   - 页面可见性感知 (隐藏时停止轮询/推送)                    │
│   - DataSource 单例替代 Provider (更轻量)                  │
│   - 火焰图 WS notify + HTTP 拉取混合模式                   │
│   - Replay 流式解析 (ReadableStream + 进度回调)            │
│   - 前端组件测试 + E2E (Playwright)                        │
│                                                          │
│   ★★★ 高优先级 (全部已完成):                               │
│   P2-A: ✅ WS 同端口方案                                  │
│   P2-B: ✅ 前端组件测试 + E2E                             │
│                                                          │
│   ★★☆ 中优先级 (全部已完成):                               │
│   P2-1: ✅ Replay 流式解析                                │
│   P2-6: ✅ 火焰图接入 LiveDataSource                      │
│                                                          │
│   ★★☆ 中优先级 (全部已完成):                               │
│   P2-C: ✅ useProcessDetail 改走 LiveDataSource            │
│   P2-D: ✅ WS 广播去重 (DataBatchPtr 指针比较)             │
│   P2-E: ✅ /pipelines API 报告活跃管道                     │
│   P2-F: ✅ 第三批前端死代码清理                            │
│                                                          │
│   ★☆☆ 低优先级 (待做):                                    │
│   P2-2/4/9~17: 各类清理、功能补全、配置修复                │
│                                                          │
│   ⚠ 剩余架构问题 (低优先级):                               │
│   - 双内存存储冗余 (StreamSinkStore + WebSocketSinkStore)  │
│   - 配置标志 http/ws_enabled 不生效                        │
│   - /stream API 无活跃状态检查                             │
│                                                          │
│   ✅ 本轮已修复的问题:                                      │
│   - useProcessDetail 绕过 WS → 改走 LiveDataSource        │
│   - /pipelines 不报告活跃管道 → 新增 active_features 字段  │
│   - WS 重复推送相同数据 → DataBatchPtr 指针去重            │
│   - 第三批死代码 (4 个符号) → 已清理                       │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

---

## 九、总结

Illuminator 是一个**设计精良的全栈观测性平台**，经过多轮清理后架构健康度显著提升，但仍存在一些结构性问题需要关注。

**后端（8.4/10）：** Pipeline v3 事件驱动架构设计精良，对标 Vector/OTel Collector。FeatureManager 提供了优雅的 on-demand 生命周期管理。7 个 eBPF 探针全部正常工作。安全性完善——HTTP 和 WS 已统一到单端口 :9527 且 Bearer Auth 保护。

主要问题：
- **双管道所有权过渡期**：`PipelineController` 持有空闲管道模板，`FeatureManager` 创建活跃管道，`/pipelines` API 只报告前者（信息不完整）
- **双内存存储冗余**：`StreamSinkStore` + `WebSocketSinkStore` 对同一数据维护两份副本
- 遗留代码：`Listen()`/`AcceptLoop()` 不再使用但未删除；配置标志不生效；错误处理不一致

**前端（8.2/10）：** 经过两轮清理后核心架构明显改善。`LiveDataSource` 全局单例 + WS/HTTP 自动降级模式设计良好。火焰图已统一为 Worker 异步计算。Replay 引擎支持流式解析大文件。

主要问题：
- **3 条独立数据路径**：7 个监控 hooks 走 WS、火焰图走 notify+HTTP、`useProcessDetail` 走独立 HTTP 轮询（最后一条造成重复流量）
- **Replay 支持不完整**：仅 CPU hooks 接受 `replaySource` 参数；IO/Network/GPU 为占位符
- **第三批死代码**：8 个未使用的符号/方法待清理（`useFeatureStream` hook、`AnnotationOverlay` 等）
- **无 profiling 停止 UX**：只有启动按钮，无对称的停止操作

**前后端通信（8.7/10）：** 同端口方案 (WsAwareServer) 实现优雅。通信模式分层合理：
- 监控类：WS 全量推送（~1s）+ HTTP 降级
- Profiling：WS 轻量 notify + HTTP cursor-based 拉取（设计合理——累积历史需求）
- 管理类：纯 HTTP 按需/低频轮询

**当前架构健康度（综合 8.4/10）：** 核心数据通路已验证正确。高优先级改进已全部完成（同端口 WS、组件测试、Replay 流式、火焰图 WS 通知）。**下一步重点**应放在：
1. 消除 `useProcessDetail` 的重复流量（P2-C）
2. 清理后端双管道所有权混淆（P2-E）
3. 清理第三批前端死代码（P2-F）

这些是目前影响架构可理解性和维护性的主要障碍。
