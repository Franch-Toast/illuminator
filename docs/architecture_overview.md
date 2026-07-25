# Illuminator 系统架构全景分析

> **版本**: 2.0  
> **日期**: 2026-07-12  
> **状态**: 当前实现 (Implemented)  
> **目标读者**: 架构师、核心开发者、技术评审

---

## 一、顶层架构全景

Illuminator 采用 **Linux 驱动模型风格的三层架构**，结合 **事件驱动管道引擎** 和 **SSE 实时推送**，
构建了一个高性能、插件化的全栈可观测性平台。

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                            ILLUMINATOR 系统架构全景                               │
│                                                                                 │
│  ┌─ 前端 (React 18 + TypeScript) ──────────────────────────────────────────────┐│
│  │                                                                             ││
│  │   Pages ─→ Hooks ─→ DataBus (SSE via SseLink) ─→ ECharts / FlameGraph     ││
│  │                                                                             ││
│  │   控制面: apiClient.ts ─→ REST /api/v2/features/*                          ││
│  │   数据面: dataBus.ts   ─→ SSE  /api/v1/events/*                            ││
│  │                                                                             ││
│  └─────────────────────────────────────────────────────────────────────────────┘│
│                              │ HTTP REST + SSE │                                 │
│  ┌───────────────────────────▼─────────────────▼───────────────────────────────┐│
│  │                         Server Layer                                         ││
│  │   HttpServer (cpp-httplib, port 9527)                                       ││
│  │   ├── API Routes: /api/v2/features/* (控制面)                               ││
│  │   ├── SseHandler: /api/v1/events/*   (数据面, 订阅制)                       ││
│  │   └── RecordingAPI: /api/v1/features/*/record/* (录制控制)                  ││
│  └───────────────────────────┬─────────────────────────────────────────────────┘│
│                              │                                                   │
│  ┌───────────────────────────▼─────────────────────────────────────────────────┐│
│  │                    Engine Layer (RFC v3 三层架构)                             ││
│  │                                                                             ││
│  │   Layer 1: InfrastructureManager ─── 基础设施 (共享资源)                     ││
│  │   Layer 2: FeatureBus            ─── 框架总线 (注册 + 编排)                  ││
│  │   Layer 3: FeatureDriver[]       ─── 功能实例 (自包含 Pipeline)              ││
│  │                                                                             ││
│  └───────────────────────────┬─────────────────────────────────────────────────┘│
│                              │                                                   │
│  ┌───────────────────────────▼─────────────────────────────────────────────────┐│
│  │                      Plugin Layer (Source / Processor / Sink)                ││
│  │   ├── Sources:      cpu_utilization, cpu_profiler, offcpu, io_monitor, ...  ││
│  │   ├── Processors:   filter, passthrough, stack_symbolizer, stack_merger     ││
│  │   ├── Aggregators:  cpu_stats_aggregator                                   ││
│  │   └── Sinks:        SseSink, RecordingSink, console, storage, ...          ││
│  └───────────────────────────┬─────────────────────────────────────────────────┘│
│                              │                                                   │
│  ┌───────────────────────────▼─────────────────────────────────────────────────┐│
│  │                         eBPF Subsystem (内核级采集)                           ││
│  │   libbpf Skeleton (嵌入字节码) + CO-RE                                      ││
│  │   ├── cpu_profiler.bpf.c    (perf_event + BPF tgid 过滤)                   ││
│  │   ├── offcpu_profiler.bpf.c  (sched tracepoint + tgid 过滤)                 ││
│  │   ├── bio_latency.bpf.c     (block I/O 追踪)                               ││
│  │   ├── net_tracer.bpf.c      (TCP/UDP 连接追踪)                             ││
│  │   └── sched_*.bpf.c         (调度器分析)                                    ││
│  └─────────────────────────────────────────────────────────────────────────────┘│
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 二、三层引擎架构详解

### 2.1 架构分层与职责边界

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│   ┌─ Layer 1: InfrastructureManager ──────────────────────────────────────┐ │
│   │                                                                       │ │
│   │   职责: 管理全局共享资源，不感知业务逻辑                                │ │
│   │                                                                       │ │
│   │   ┌───────────────┐  ┌───────────────┐  ┌───────────────┐            │ │
│   │   │  TimerWheel   │  │  CollectPool  │  │   SinkPool    │            │ │
│   │   │  (1 线程)     │  │  (M 线程)     │  │  (K 线程)     │            │ │
│   │   │              │  │              │  │              │            │ │
│   │   │ timerfd+epoll │  │ 并行 Collect │  │ 并行 Write  │            │ │
│   │   │ 精准定时触发  │  │ I/O 隔离     │  │ I/O 隔离    │            │ │
│   │   └───────────────┘  └───────────────┘  └───────────────┘            │ │
│   │                                                                       │ │
│   │   不知道: Feature / Pipeline / Plugin                                 │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
│                                      │                                       │
│   ┌─ Layer 2: FeatureBus ────────────▼────────────────────────────────────┐ │
│   │                                                                       │ │
│   │   职责: 注册 Driver, 编排生命周期, 提供统一查询接口                     │ │
│   │                                                                       │ │
│   │   ┌────────────────────────────────────────────────────────────────┐  │ │
│   │   │  drivers_: Map<name, unique_ptr<FeatureDriver>>                │  │ │
│   │   │                                                                │  │ │
│   │   │  API:                                                          │  │ │
│   │   │   Register(driver) → Probe(name) → Pause/Resume → Remove(name)│  │ │
│   │   │   ProbeAll() → ListDrivers() → GetState() → SetConfig()       │  │ │
│   │   │                                                                │  │ │
│   │   │  Auto-Probe: Tier 1-2 在 daemon 启动时自动 Probe              │  │ │
│   │   └────────────────────────────────────────────────────────────────┘  │ │
│   │                                                                       │ │
│   │   不知道: Plugin 内部实现 / Pipeline 数据如何处理                      │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
│                                      │                                       │
│   ┌─ Layer 3: FeatureDriver[] ───────▼────────────────────────────────────┐ │
│   │                                                                       │ │
│   │   职责: 每个 Feature 自包含，构建并管理自己的 Pipeline                  │ │
│   │                                                                       │ │
│   │   ┌──────────────────┐ ┌──────────────────┐ ┌──────────────────┐     │ │
│   │   │ CpuUtilization   │ │ CpuProfiler      │ │ IoMonitor        │     │ │
│   │   │ Driver           │ │ Driver           │ │ Driver           │     │ │
│   │   │                  │ │                  │ │                  │     │ │
│   │   │ BuildPipeline()  │ │ BuildPipeline()  │ │ BuildPipeline()  │     │ │
│   │   │ ├─ Source        │ │ ├─ eBPF Source   │ │ ├─ Source        │     │ │
│   │   │ ├─ Processor[]   │ │ ├─ Symbolizer   │ │ ├─ Processor[]   │     │ │
│   │   │ └─ SseSink       │ │ ├─ Merger       │ │ └─ SseSink       │     │ │
│   │   │                  │ │ └─ SseSink       │ │                  │     │ │
│   │   └──────────────────┘ └──────────────────┘ └──────────────────┘     │ │
│   │                                                                       │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 与 Linux 驱动模型的精确对照

```
┌────────────────────────────────────────────────────────────────────────────┐
│                    Linux Kernel                │     Illuminator            │
├────────────────────────────────────────────────┼────────────────────────────┤
│                                               │                            │
│  struct bus_type {                             │  class FeatureBus {        │
│      .match = bus_match_driver                │      Register()            │
│      .probe = bus_probe_driver                │      Probe() / Remove()    │
│      .remove = bus_remove_driver              │      ProbeAll()            │
│  }                                            │  }                         │
│                                               │                            │
│  struct device_driver {                       │  class FeatureDriver {     │
│      .probe = driver_probe                    │      Probe()               │
│      .remove = driver_remove                  │      Remove()              │
│      .suspend = driver_suspend                │      Pause()               │
│      .resume = driver_resume                  │      Resume()              │
│  }                                            │  }                         │
│                                               │                            │
│  struct device {                              │  Feature 实例 (运行时)      │
│      .dev_attrs                               │      Describe() → Descriptor│
│      .driver_data                             │      pipeline_ (成员)      │
│  }                                            │                            │
│                                               │                            │
│  module_init() / platform_driver_register()   │  REGISTER_FEATURE()        │
│                                               │                            │
│  /sys/class/                                  │  REST /api/v2/features/*   │
│  /proc/                                       │  SSE /api/v1/events/*      │
│                                               │                            │
│  devm_*() 自动资源管理                         │  Probe() 中自动注入 Sink   │
│                                               │  Remove() 自动清理定时器   │
│                                               │                            │
└────────────────────────────────────────────────┴────────────────────────────┘
```

**核心类比关系**:

| Linux 概念 | Illuminator 对应 | 行为映射 |
|-----------|-----------------|---------|
| `bus_type` | `FeatureBus` | 设备总线，管理所有驱动的注册和匹配 |
| `device_driver` | `FeatureDriver` 基类 | 驱动抽象，定义标准 probe/remove 接口 |
| `platform_device` | Feature 实例 (runtime) | 设备实例，每个 Feature 是一个虚拟设备 |
| `probe()` | `FeatureDriver::Probe()` | 初始化设备 = 构建 Pipeline + 注册定时器 + 启动 |
| `remove()` | `FeatureDriver::Remove()` | 释放设备 = 停止 Pipeline + 注销定时器 + 清理 |
| `suspend/resume` | `Pause()/Resume()` | 暂停/恢复采集（取消/重注册定时器） |
| `driver_register()` | `REGISTER_FEATURE()` 宏 | 编译时自动注册到 FeatureRegistry |
| `device_attribute` | `FeatureDescriptor` | 设备自描述（name, tier, category, params） |
| `sysfs` | REST API `/api/v2/features/*` | 对外暴露统一控制接口 |
| `uevent` | `StateChangeCallback` | 设备状态变更通知 |
| `devm_*()` 自动管理 | 基类 `Probe()` 自动注入 | RecordingSink + 定时器注册由基类统一处理 |

---

## 三、Feature 生命周期状态机

### 3.1 状态转换图

```
                  FeatureBus::ProbeAll()
                  (Tier 1-2 自动)
                         │
                         ▼
     ┌──────────────────────────────────────────────────────────┐
     │                                                          │
     │   ┌──────────┐    Probe()     ┌──────────┐              │
     │   │          │ ──────────────→│          │              │
     │   │ Inactive │                │  Active  │◄─── Resume() │
     │   │          │←──────────────│          │              │
     │   └──────────┘    Remove()    └─────┬────┘              │
     │        ▲                            │                    │
     │        │                         Pause()                 │
     │        │                            │                    │
     │        │                            ▼                    │
     │        │          Remove()    ┌──────────┐              │
     │        └──────────────────────│  Paused  │              │
     │                               └──────────┘              │
     │                                                          │
     └──────────────────────────────────────────────────────────┘
```

### 3.2 Probe() 内部执行序列

```
FeatureDriver::Probe()
    │
    ├─ 1. 检查状态 (必须 kInactive)
    │
    ├─ 2. 检查 InfrastructureManager 是否已启动
    │
    ├─ 3. BuildPipeline(infra) ← 子类实现，构建 Source + Processor + Sink 链
    │     │
    │     ├─ 创建 Source 插件 (Pull 或 Push 模式)
    │     ├─ 创建 Processor 链 (可选: filter, symbolizer, merger)
    │     └─ 附加 SseSink (SSE 实时推送)
    │
    ├─ 4. 自动注入 RecordingSink (on-demand 文件 I/O)
    │     └─ RecordingSinkRegistry::Register(Name(), sink)
    │
    ├─ 5. pipeline_->SetSinkPool(infra.GetSinkPool())
    │
    ├─ 6. pipeline_->Start() ← 启动 ProcessThread
    │
    ├─ 7. RegisterTimers(infra) ← 基类默认实现
    │     ├─ Pull Source: TimerWheel::AddRepeating → CollectPool::Submit(Collect)
    │     └─ Aggregator: TimerWheel::AddRepeating → pipeline_->InjectFlush()
    │
    └─ 8. state_ = kActive
```

### 3.3 分层自动激活策略

```
daemon 启动
    │
    ├── InfrastructureManager::Start()
    │     ├── TimerWheel 线程启动
    │     ├── CollectPool 线程启动 (默认 2 线程)
    │     └── SinkPool 线程启动 (默认 CPU/2 线程)
    │
    ├── FeatureRegistry::RegisterAll()
    │     └── 所有 REGISTER_FEATURE() 注册的 Driver 加入 FeatureBus
    │
    └── FeatureBus::ProbeAll()
          │
          ├── Tier 1 (kMonitoring): 自动 Probe ← 无需用户干预
          │     └── cpu_utilization, process_cpu
          │
          ├── Tier 2 (kTracing): 自动 Probe ← 轻量 eBPF
          │     └── io_monitor, net_tracer, sched_analyzer
          │
          └── Tier 3 (kProfiling): 不自动 Probe ← 需手动触发
                └── cpu_profiler, offcpu_profiler (等待 REST API 调用)
```

---

## 四、Pipeline 内部数据流

### 4.1 Pipeline 组件结构

```
┌─ Pipeline (每个 FeatureDriver 持有一个) ─────────────────────────────────────┐
│                                                                              │
│   ┌─────────────┐                                                            │
│   │   Source    │ ─── Pull 模式: CollectPool 定时调用 Collect()               │
│   │   Plugin    │ ─── Push 模式: eBPF callback 直接 Enqueue                  │
│   └──────┬──────┘                                                            │
│          │ TryEnqueue(DataBatchPtr)                                           │
│          ▼                                                                    │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │  AsyncChannel<variant<DataBatchPtr, FlushSentinel>>                   │   │
│   │  无锁环形缓冲区, 4096 slots, 三级自适应退避出队                         │   │
│   │  反压: 80% 高水位触发, 20% 低水位解除, 滞后设计防震荡                    │   │
│   └──────────────────────────────────────┬───────────────────────────────┘   │
│                                          │ Dequeue (spin → yield → sleep)    │
│                                          ▼                                    │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │  ProcessThread (1 per Pipeline, 独占线程)                              │   │
│   │                                                                      │   │
│   │  while (running_) {                                                  │   │
│   │      item = channel.Dequeue(100ms)                                   │   │
│   │      match item:                                                     │   │
│   │        DataBatch  → Processors[] → Aggregator.Add() → SubmitToSinks  │   │
│   │        Sentinel   → Aggregator.Flush() → SubmitToSinks               │   │
│   │  }                                                                   │   │
│   │                                                                      │   │
│   │  约束: 纯 CPU-bound, 不做任何 I/O, 不持有定时器                        │   │
│   └──────────────────────────────────────┬───────────────────────────────┘   │
│                                          │ SubmitToSinks()                    │
│                                          ▼                                    │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │  Sink 列表 (通过 SinkPool 并行执行 Write)                              │   │
│   │                                                                      │   │
│   │  ├── SseSink ────────→ SseHandler::Publish() → SSE 推送到前端        │   │
│   │  ├── RecordingSink ──→ 文件 I/O (.ilr 格式落盘, on-demand)           │   │
│   │  └── [其他 Sink] ───→ console / storage / ...                        │   │
│   │                                                                      │   │
│   │  过载保护: SinkPool pending > 256 → 丢弃当前 batch                    │   │
│   └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Pull Source 数据流时序

```
TimerWheel       CollectPool      AsyncChannel     ProcessThread     SinkPool        SseHandler
    │                │                │                │                │                │
    │ timer fires    │                │                │                │                │
    │───┐            │                │                │                │                │
    │   │ 回调:      │                │                │                │                │
    │   │ Submit()   │                │                │                │                │
    │───┼───────────→│                │                │                │                │
    │                │                │                │                │                │
    │                │ Collect()      │                │                │                │
    │                │──┐ 读 /proc    │                │                │                │
    │                │←─┘             │                │                │                │
    │                │                │                │                │                │
    │                │ TryEnqueue()   │                │                │                │
    │                │───────────────→│                │                │                │
    │                │  (CAS, ns级)   │                │                │                │
    │                │                │ Dequeue()      │                │                │
    │                │                │───────────────→│                │                │
    │                │                │ (三级退避)      │                │                │
    │                │                │                │                │                │
    │                │                │                │ RunProcessors() │                │
    │                │                │                │──┐              │                │
    │                │                │                │←─┘              │                │
    │                │                │                │                │                │
    │                │                │                │ SubmitToSinks() │                │
    │                │                │                │───────────────→│                │
    │                │                │                │                │ SseSink.Write()│
    │                │                │                │                │───────────────→│
    │                │                │                │                │                │
    │                │                │                │                │                │ SSE push
    │                │                │                │                │                │──→ 前端
```

### 4.3 Push Source (eBPF) 数据流时序

```
Linux Kernel          eBPF ringbuf         CpuProfilerSource    AsyncChannel     ProcessThread
    │                      │                      │                  │                │
    │ perf_event 中断       │                      │                  │                │
    │──→ BPF 程序执行       │                      │                  │                │
    │    tgid 过滤 ✓        │                      │                  │                │
    │    采样 stack trace   │                      │                  │                │
    │──→ bpf_ringbuf_output │                      │                  │                │
    │                      │                      │                  │                │
    │                      │ ring_buffer callback  │                  │                │
    │                      │─────────────────────→│                  │                │
    │                      │                      │                  │                │
    │                      │                      │ Enqueue(batch)   │                │
    │                      │                      │─────────────────→│                │
    │                      │                      │  (无锁 CAS)      │                │
    │                      │                      │                  │ Dequeue()      │
    │                      │                      │                  │───────────────→│
    │                      │                      │                  │                │
    │                      │                      │                  │ Symbolize +    │
    │                      │                      │                  │ StackMerge     │
    │                      │                      │                  │ → SseSink      │
    │                      │                      │                  │ → 前端火焰图    │
```

### 4.4 eBPF Source 架构（EbpfSourceBase）

所有 eBPF Source 插件统一继承 `EbpfSourceBase`，采用 Linux 驱动框架风格的
"框架 + hooks" 设计模式。PID/Comm 过滤、perf_event 管理等能力通过 `bpf_util`
工具函数按需调用（类似 Linux `devm_*`），基类不强制使用。

```
SourcePlugin (抽象基类)
│
└── EbpfSourceBase                       统一 eBPF Source 基类
    │  - Skeleton 生命周期: open → configure → load → attach → destroy
    │  - Stub 模式: 内核不支持 BPF 时优雅降级
    │  - Gate 管理 + MetaStats 自观测
    │  - IL_SKEL_CALLBACKS(skel_name) 宏: 一行生成 skeleton 回调表
    │
    │  Push 模式: ConsumeAndBatch() — ring_buffer__consume() 批量排空
    │  Pull 模式: Collect() → CollectFromMaps() 从 BPF maps 读聚合数据
    │  无独立 poll 线程 — 由 Pipeline 引擎 (TimerWheel + CollectPool) 统一调度
    │
    │  子类 Hooks (类似 Linux struct xxx_ops):
    │    OnConfigureRodata()   — load 前写 rodata
    │    OnConfigureMaps()     — load 后配置 BPF maps
    │    OnPostAttach()        — attach 后额外挂载 (如 perf_event)
    │    OnPreDestroy()        — 停止时清理额外资源
    │    GetEventCallback()    — Push: ring buffer 事件回调
    │    CollectFromMaps()     — Pull: 从 maps 读聚合数据
    │
    ├── EbpfIoMonitor          (70 行)   Push — bio tracepoint
    ├── EbpfNetTracer          (81 行)   Push — inet_sock tracepoint
    ├── EbpfSchedTracer        (82 行)   Push — sched tracepoint
    ├── CpuProfilerSource     (337 行)   Pull/Push — perf_event 采样
    ├── OffcpuProfilerSource  (496 行)   Pull — off-CPU 分析 + 符号解析
    └── SchedAnalyzerSource   (403 行)   Pull/Push — 调度聚合 + 详细事件
```

**bpf_util 工具函数** (`src/ebpf_common/loader/bpf_util.h`):
```
bpf_util::WritePidFilter()          — 写入 PID 白名单到 BPF hash map
bpf_util::WriteCommFilter()         — 写入进程名白名单
bpf_util::ConfigurePidNamespace()   — 配置 PID namespace (bpf_get_ns_current_pid_tgid)
bpf_util::AttachPerfEvents()        — 为所有在线 CPU 创建并挂载 perf_event
bpf_util::DetachPerfEvents()        — 关闭所有 perf_event fd
bpf_util::EnablePerfEvents()        — ioctl PERF_EVENT_IOC_ENABLE
bpf_util::DisablePerfEvents()       — ioctl PERF_EVENT_IOC_DISABLE
bpf_util::AdjustPerfFrequency()     — 运行时调整采样频率 (反压)
```

**Push vs Pull 数据流对比**:

```
Push (EbpfSourceBase + ConsumeAndBatch):
  内核 BPF → ring buffer → TimerWheel 定时触发
           → ring_buffer__consume() 批量排空 → pending_batch_ → pipeline

Pull (EbpfSourceBase + CollectFromMaps):
  内核 BPF → 内核 map [聚合统计]
           → TimerWheel Collect() → CollectFromMaps() → pipeline
```

---

## 五、前后端交互完整链路

### 5.1 双通道架构

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                               前后端通信架构                                      │
│                                                                                 │
│   ┌─ 控制面 (REST, 请求-响应) ─────────────────────────────────────────────┐    │
│   │                                                                        │    │
│   │   前端 apiClient.ts                                                    │    │
│   │     │                                                                  │    │
│   │     ├─ GET  /api/v2/features              → Feature 列表 + 元数据      │    │
│   │     ├─ POST /api/v2/features/:name/start  → FeatureBus::Probe()       │    │
│   │     ├─ POST /api/v2/features/:name/stop   → FeatureBus::Remove()      │    │
│   │     ├─ POST /api/v2/features/:name/pause  → FeatureBus::Pause()       │    │
│   │     ├─ POST /api/v2/features/:name/resume → FeatureBus::Resume()      │    │
│   │     ├─ GET  /api/v2/features/:name/config/schema → JSON Schema        │    │
│   │     ├─ POST /api/v2/features/:name/config → FeatureDriver::SetConfig()│    │
│   │     └─ POST /api/v1/features/:name/record/start|stop → 录制控制       │    │
│   │                                                                        │    │
│   └────────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
│   ┌─ 数据面 (SSE, 服务器单向推送) ─────────────────────────────────────────┐    │
│   │                                                                        │    │
│   │   前端 dataBus.ts + sseLink.ts                                         │    │
│   │     │                                                                  │    │
│   │     ├─ Step 1: POST /api/v1/events/subscribe                           │    │
│   │     │          Body: { features: ["cpu_utilization", "process_cpu"] }   │    │
│   │     │          → Response: { subscription_id, url }                    │    │
│   │     │                                                                  │    │
│   │     ├─ Step 2: GET /api/v1/events/{subscription_id}                    │    │
│   │     │          → SSE 长连接 (Content-Type: text/event-stream)          │    │
│   │     │          → event: data\ndata: {json}\n\n                         │    │
│   │     │          → event: frame\ndata: {frame_json}\n\n (>64KB 分帧)     │    │
│   │     │          → : keepalive\n\n (每 15s 心跳)                         │    │
│   │     │                                                                  │    │
│   │     └─ Step 3: POST /api/v1/events/{id}/update (动态增删订阅)          │    │
│   │                Body: { add: ["io_monitor"], remove: ["process_cpu"] }   │    │
│   │                                                                        │    │
│   └────────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 5.2 SSE 数据推送端到端链路

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                        SSE 数据推送完整链路                                       │
│                                                                                 │
│  eBPF/procfs 采集                                                               │
│       │                                                                         │
│       ▼                                                                         │
│  Source::Collect() / eBPF callback                                              │
│       │                                                                         │
│       ▼                                                                         │
│  AsyncChannel (无锁队列, 4096 slots)                                             │
│       │                                                                         │
│       ▼                                                                         │
│  ProcessThread → Processor[] → Aggregator                                       │
│       │                                                                         │
│       ▼                                                                         │
│  SubmitToSinks() ──→ SinkPool 线程执行                                           │
│       │                                                                         │
│       ▼                                                                         │
│  SseSink::Write(batch)                                                          │
│       │                                                                         │
│       │  push 到 SseHandler 内部 per-subscription outbox                        │
│       │  (内存操作 + condition_variable 通知, 微秒级)                            │
│       ▼                                                                         │
│  SseHandler::Push()                                                             │
│       │                                                                         │
│       │  序列化 DataBatch → JSON                                                │
│       │  如果 > 64KB → 自动 frame splitting                                     │
│       │  格式: "event: data\ndata: {json}\n\n"                                  │
│       ▼                                                                         │
│  httplib HTTP 线程 → write(fd) → TCP socket                                     │
│       │                                                                         │
│       │ ─────────────── 网络传输 ───────────────                                │
│       ▼                                                                         │
│  Browser EventSource (SseLink 封装)                                             │
│       │                                                                         │
│       │  addEventListener('data', handler)                                      │
│       │  addEventListener('frame', handler) ← 分帧重组                          │
│       ▼                                                                         │
│  DataBus.handleData(feature, payload)                                           │
│       │                                                                         │
│       ├─ 写入 per-feature ringBuffer (最近 300 条)                              │
│       ├─ 通知所有 subscribers                                                   │
│       ▼                                                                         │
│  Hook callback (useCpuData / useProfileData / ...)                              │
│       │                                                                         │
│       ├─ extractRecords(batch.data) ← 解析 metrics[] 或 records[]               │
│       ├─ setState(newData) → React re-render                                    │
│       ▼                                                                         │
│  ECharts setOption() / FlameGraph Worker                                        │
│       │                                                                         │
│       └─→ 用户可见的实时图表更新                                                 │
│                                                                                 │
│  延迟分析:                                                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐        │
│  │ Collect (1-50ms) + Channel (ns-ms) + Process (μs)                   │        │
│  │ + SinkPool→SseHandler (μs) + Network (<1ms local)                   │        │
│  │ + EventSource parse + Hook + Render                                 │        │
│  │ = 端到端 < 100ms (本地, 典型 10-50ms)                               │        │
│  └─────────────────────────────────────────────────────────────────────┘        │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 5.3 前端数据消费架构

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                         前端数据消费分层架构                                      │
│                                                                                 │
│  ┌─ 传输层 ──────────────────────────────────────────────────────────────────┐  │
│  │                                                                           │  │
│  │  SseLink (sseLink.ts)                                                     │  │
│  │  ├── EventSource 封装 (浏览器原生 SSE API)                                 │  │
│  │  ├── 自动重连: 指数退避 1s → 2s → 4s → ... → 30s max                      │  │
│  │  ├── 事件分发: "data" / "frame" / "open" / "error"                        │  │
│  │  └── 连接状态管理: connected / disconnected / reconnecting                │  │
│  │                                                                           │  │
│  └───────────────────────────────────┬───────────────────────────────────────┘  │
│                                      │                                           │
│  ┌─ 订阅管理层 ─────────────────────▼───────────────────────────────────────┐  │
│  │                                                                           │  │
│  │  DataBus (dataBus.ts, 全局单例, 实现 DataSource 接口)                      │  │
│  │  ├── connect() → POST /subscribe + SseLink.connect()                      │  │
│  │  ├── subscribe(feature, cb) → 维护 Map<feature, Set<callback>>            │  │
│  │  │   └── syncSubscription() → POST /update (增删 SSE feature 过滤)        │  │
│  │  ├── handleData(feature, payload) → 解析 + 分发 + ringBuffer 缓存         │  │
│  │  ├── handleFrame(frame) → 分帧重组 (seq + frame_idx + frame_total)        │  │
│  │  ├── getLatest(feature) → 返回最近一条 DataBatch                           │  │
│  │  └── getAvailableFeatures() → 返回已订阅 feature 列表                      │  │
│  │                                                                           │  │
│  └───────────────────────────────────┬───────────────────────────────────────┘  │
│                                      │                                           │
│  ┌─ 业务层 ─────────────────────────▼───────────────────────────────────────┐  │
│  │                                                                           │  │
│  │  Data Hooks (各页面使用)                                                   │  │
│  │  ├── useCpuData(active, replaySource?)                                    │  │
│  │  │   └── getDataSource().subscribe('cpu_utilization', cb)                 │  │
│  │  ├── useProfileData(pid, active, replaySource?)                           │  │
│  │  │   └── getDataSource().subscribe('cpu_profiler', cb)                    │  │
│  │  ├── useIoData(active, replaySource?)                                     │  │
│  │  └── ...                                                                  │  │
│  │                                                                           │  │
│  │  DataSource 接口抽象:                                                      │  │
│  │  ├── Live 模式: getDataSource() → DataBus 单例 (SSE)                      │  │
│  │  └── Replay 模式: replaySource → ReplayEngine (.ilr 文件解析)             │  │
│  │                                                                           │  │
│  └───────────────────────────────────┬───────────────────────────────────────┘  │
│                                      │                                           │
│  ┌─ 渲染层 ─────────────────────────▼───────────────────────────────────────┐  │
│  │                                                                           │  │
│  │  React 组件                                                               │  │
│  │  ├── ECharts: 时序面积图、热力图、直方图 (Canvas 渲染, 60fps)              │  │
│  │  ├── FlameGraph: Web Worker 异步构建 + CSS div 渲染                       │  │
│  │  ├── SummaryCard: 实时摘要指标卡片                                         │  │
│  │  └── ProcessTable: 虚拟滚动 (>40 行自动启用)                               │  │
│  │                                                                           │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 六、线程模型

### 6.1 线程分工全景

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                            线程模型 (典型 10 管道配置)                             │
│                                                                                 │
│  ┌─ 调度层 (1 线程) ────────────────────────────────────────────────────────┐   │
│  │  timer-wheel: timerfd + epoll + eventfd                                  │   │
│  │  职责: 精准定时，不执行实际工作，只做 Submit/Enqueue                       │   │
│  │  延迟: 纳秒级回调执行                                                     │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  ┌─ 采集层 (M=2 线程) ─────────────────────────────────────────────────────┐   │
│  │  collect-pool-0, collect-pool-1                                          │   │
│  │  职责: 并行执行 Source::Collect() (读 /proc, 系统调用)                    │   │
│  │  特点: I/O-bound, 一个慢 Collect 不阻塞其他 Source                        │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  ┌─ 处理层 (N=10 线程, 每 Pipeline 1 个) ──────────────────────────────────┐   │
│  │  process-cpu_util, process-process_cpu, process-cpu_profiler, ...        │   │
│  │  职责: 纯事件处理器 (Processor 链 + Aggregator)                           │   │
│  │  特点: CPU-bound, 无任何 I/O, 无锁(独占线程串行访问有状态组件)            │   │
│  │  退避: 三级自适应 (spin 16 → yield 8 → sleep 1ms)                        │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  ┌─ 写入层 (K=4 线程) ─────────────────────────────────────────────────────┐   │
│  │  sink-pool-0, sink-pool-1, sink-pool-2, sink-pool-3                     │   │
│  │  职责: 并行执行 Sink::Write() (SSE push, file I/O, console)              │   │
│  │  特点: I/O-bound, SseSink 只做内存操作(push+notify), 不写 fd             │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  ┌─ HTTP 层 (httplib 线程池) ──────────────────────────────────────────────┐   │
│  │  http-worker-0, http-worker-1, ...                                      │   │
│  │  职责: 处理 REST 请求 + SSE 长连接的 write(fd)                            │   │
│  │  特点: SSE 长连接在此阻塞等待数据; 慢客户端只阻塞 HTTP 线程不影响 SinkPool │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  总计: 1 + 2 + 10 + 4 + httplib = 17+ 线程                                     │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 6.2 线程隔离保证

| 组件 | 阻塞源 | 最大阻塞 | 影响范围 |
|------|--------|---------|---------|
| TimerWheel | epoll_wait | 1s 超时 | 无影响（不做实际工作） |
| CollectPool | Source::Collect() 读 /proc | 1-50ms | 不影响其他 Source |
| ProcessThread | Dequeue 超时 | 100ms | 不影响其他 Pipeline |
| ProcessThread | RunProcessors() | ~1-100μs | 不影响（设计约束: Processor 必须快） |
| SinkPool | SseSink::Write() | ~1μs (push+notify) | 不影响（只做内存操作） |
| SinkPool | RecordingSink::Write() | 1-100ms (file I/O) | 不影响其他 Sink Worker |
| HTTP 线程 | SSE write(fd) | 可能长时间 | 只阻塞该 HTTP 线程，不影响 SinkPool |

**关键设计**: SseSink 在 SinkPool 线程中只做 `push(batch) + cv.notify_one()` (微秒级内存操作)，
真正的 `write(fd)` 发生在 httplib 的 HTTP 请求处理线程中。即使客户端 TCP 缓冲区满导致 `write` 阻塞，
阻塞的也是 HTTP 线程，SinkPool 完全不受影响。

---

## 七、BPF 内核态数据采集

### 7.1 CPU Profiler 数据路径

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                       CPU Profiler BPF 数据采集路径                               │
│                                                                                 │
│  ┌─ 内核态 ────────────────────────────────────────────────────────────────┐    │
│  │                                                                         │    │
│  │   perf_event (per-CPU, 系统级采样, 49Hz)                                │    │
│  │       │                                                                 │    │
│  │       ▼                                                                 │    │
│  │   BPF 程序 (cpu_profiler.bpf.c)                                         │    │
│  │       │                                                                 │    │
│  │       ├─ 读取 bpf_get_current_pid_tgid() → 获取当前进程 tgid            │    │
│  │       │                                                                 │    │
│  │       ├─ 查询 BPF MAP (filter_pids_map):                               │    │
│  │       │   if (filter_enabled && !bpf_map_lookup_elem(tgid)):            │    │
│  │       │       return 0;  // ← 内核态丢弃，零开销                         │    │
│  │       │                                                                 │    │
│  │       ├─ bpf_get_stackid() → 内核栈 + 用户栈                           │    │
│  │       │                                                                 │    │
│  │       └─ bpf_ringbuf_output() → 写入事件到 ring buffer                  │    │
│  │                                                                         │    │
│  └──────────────────────────────────────┬──────────────────────────────────┘    │
│                                         │ ring buffer callback                   │
│  ┌──────────────────────────────────────▼──────────────────────────────────┐    │
│  │                                                                         │    │
│  │  用户态 CpuProfilerSource                                               │    │
│  │                                                                         │    │
│  │  Start():                                                               │    │
│  │    1. cpu_profiler_sk_bpf__open() → 打开嵌入的 skeleton                 │    │
│  │    2. cpu_profiler_sk_bpf__load() → 加载 BPF 程序                       │    │
│  │    3. per-CPU 创建 perf_event_open(PERF_TYPE_SOFTWARE, freq=49)         │    │
│  │    4. bpf_program__attach_perf_event() → 附加到所有 CPU                  │    │
│  │    5. ApplyFilterMaps(): 通过 skel_->maps.xxx 直接写入                  │    │
│  │    6. ring_buffer__new() → 注册 callback                               │    │
│  │                                                                         │    │
│  │  Callback (Push 模式):                                                  │    │
│  │    解析事件 → 构建 StackSample → DataBatch → Enqueue(channel)           │    │
│  │                                                                         │    │
│  └─────────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
│  优势: 内核态 tgid 过滤 vs 用户态过滤                                            │
│  ┌─────────────────────────────────────────────────────────────────────┐        │
│  │  内核态过滤:                                                         │        │
│  │    - 采样不在目标 PID → BPF 程序直接 return 0                        │        │
│  │    - 不读取 stack trace, 不写 ring buffer                            │        │
│  │    - 系统开销: 仅目标进程的采样消耗                                   │        │
│  │                                                                     │        │
│  │  用户态过滤 (被拒绝的方案):                                           │        │
│  │    - 所有进程都采样 → 全部写入 ring buffer → 用户态丢弃               │        │
│  │    - 系统开销: 全系统 CPU 开销 (49Hz × 所有 CPU × 所有进程)           │        │
│  │    - 数据量: 可能是目标进程的 100-1000 倍                             │        │
│  └─────────────────────────────────────────────────────────────────────┘        │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 7.2 Off-CPU Profiler 数据路径

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                      Off-CPU Profiler BPF 数据采集路径                            │
│                                                                                 │
│  ┌─ 内核态 ────────────────────────────────────────────────────────────────┐    │
│  │                                                                         │    │
│  │   Tracepoint: sched_switch (调度切换时触发)                              │    │
│  │       │                                                                 │    │
│  │       ├─ 进程被调度出 (prev):                                            │    │
│  │       │   tgid = prev->tgid                                             │    │
│  │       │   if (filter_enabled && !map_lookup(tgid)): return 0            │    │
│  │       │   记录开始时间: start_map[tid] = bpf_ktime_get_ns()             │    │
│  │       │                                                                 │    │
│  │       └─ 进程被调度回 (next):                                            │    │
│  │           tid = next->pid                                               │    │
│  │           delta = now - start_map[tid]                                  │    │
│  │           if (delta > threshold):                                       │    │
│  │               采集 stack trace + 写 ringbuf                             │    │
│  │                                                                         │    │
│  │   特点: 使用 tracepoint (非 perf_event)，天然支持 tgid 过滤             │    │
│  │                                                                         │    │
│  └─────────────────────────────────────────────────────────────────────────┘    │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 八、Feature 自动发现与注册

### 8.1 编译时注册流程

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                        Feature 自动注册机制                                       │
│                                                                                 │
│  源代码 (编译时):                                                                │
│                                                                                 │
│    cpu_utilization_driver.h:                                                    │
│    ┌────────────────────────────────────────────────────────────────┐           │
│    │  class CpuUtilizationDriver : public FeatureDriver { ... };    │           │
│    │  REGISTER_FEATURE(CpuUtilizationDriver);                       │           │
│    │                                                                │           │
│    │  // 宏展开为:                                                   │           │
│    │  static const bool _CpuUtilizationDriver_registered = []() {   │           │
│    │      FeatureRegistry::Instance().Add(                           │           │
│    │          std::make_unique<CpuUtilizationDriver>());             │           │
│    │      return true;                                              │           │
│    │  }();                                                          │           │
│    └────────────────────────────────────────────────────────────────┘           │
│                                                                                 │
│  运行时 (daemon 启动):                                                           │
│                                                                                 │
│    main.cc → RunDaemon():                                                       │
│    ┌────────────────────────────────────────────────────────────────┐           │
│    │  1. InfrastructureManager::Instance().Start()                   │           │
│    │                                                                │           │
│    │  2. FeatureRegistry::RegisterAll()                              │           │
│    │     └─ 遍历所有已注册 Driver → FeatureBus::Register(driver)    │           │
│    │                                                                │           │
│    │  3. FeatureBus::ProbeAll()                                      │           │
│    │     ├─ Tier 1 (kMonitoring): 自动 Probe (cpu_util, process_cpu)│           │
│    │     ├─ Tier 2 (kTracing):    自动 Probe (io, net, sched)       │           │
│    │     └─ Tier 3 (kProfiling):  跳过 (等 REST API 触发)           │           │
│    │                                                                │           │
│    │  4. HttpServer::Start()                                         │           │
│    │     └─ 开始接受请求 + SSE 连接                                  │           │
│    └────────────────────────────────────────────────────────────────┘           │
│                                                                                 │
│  新增 Feature 零改动框架代码:                                                    │
│    1. 写一个 FeatureDriver 子类 (50-70 行)                                      │
│    2. 加 REGISTER_FEATURE() 宏                                                  │
│    3. 添加 BUILD 规则 (alwayslink = True)                                       │
│    → 前端自动发现 (GET /api/v2/features 返回新 Feature 元数据)                   │
│    → 前端自动生成配置表单 (JSON Schema)                                          │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 前端自动发现链路

```
前端 App 初始化
    │
    ├─ useFeatureList() hook
    │   └─ GET /api/v2/features
    │       → 返回所有已注册 FeatureDriver 的 Descriptor
    │       → { features: [{ name, display_name, category, tier, params, ... }] }
    │
    ├─ 自动生成导航标签页
    │   └─ 按 category 分组: cpu / memory / io / network / gpu / system
    │
    ├─ 自动生成配置面板
    │   └─ GET /api/v2/features/:name/config/schema → JSON Schema
    │       → JsonSchemaForm 组件自动渲染输入控件
    │
    └─ 自动订阅 SSE 数据
        └─ DataBus.subscribe(feature.name, cb)
            → POST /api/v1/events/subscribe → SSE 长连接
```

---

## 九、典型交互场景时序图

### 9.1 用户启动 CPU Profile

```
┌───────┐          ┌─────────┐         ┌───────────┐        ┌──────────────┐
│ 用户  │          │  前端   │         │  后端API  │        │  FeatureBus  │
└───┬───┘          └────┬────┘         └─────┬─────┘        └──────┬───────┘
    │                   │                    │                      │
    │ 点击 "Start       │                    │                      │
    │  CPU Profile"     │                    │                      │
    │──────────────────→│                    │                      │
    │                   │                    │                      │
    │                   │ POST /api/v2/      │                      │
    │                   │ features/          │                      │
    │                   │ cpu_profiler/start  │                      │
    │                   │ {target_pids:[1234]}│                      │
    │                   │───────────────────→│                      │
    │                   │                    │                      │
    │                   │                    │ Probe("cpu_profiler") │
    │                   │                    │─────────────────────→│
    │                   │                    │                      │
    │                   │                    │      ┌───────────────┤
    │                   │                    │      │ BuildPipeline  │
    │                   │                    │      │ ├ BPF load     │
    │                   │                    │      │ ├ perf_event   │
    │                   │                    │      │ ├ tgid filter  │
    │                   │                    │      │ └ SseSink      │
    │                   │                    │      │                │
    │                   │                    │      │ Pipeline.Start │
    │                   │                    │      └───────────────┤
    │                   │                    │                      │
    │                   │                    │←─ Status::Ok() ──────│
    │                   │←── 200 OK ─────────│                      │
    │                   │                    │                      │
    │←─ UI 更新:        │                    │                      │
    │   "Profiling..."  │                    │                      │
    │                   │                    │                      │
    │                   │     SSE: profile data (持续推送)          │
    │                   │←═══════════════════╪══════════════════════│
    │                   │                    │                      │
    │←─ 火焰图实时       │                    │                      │
    │   累积更新         │                    │                      │
```

### 9.2 Always-On 数据自动流动

```
┌──────────┐      ┌────────────┐      ┌───────────┐      ┌──────────┐
│  daemon  │      │  FeatureBus│      │ Pipeline  │      │ SseHandler│
│  启动    │      │            │      │ (多个)    │      │          │
└────┬─────┘      └──────┬─────┘      └─────┬─────┘      └─────┬────┘
     │                   │                   │                   │
     │ ProbeAll()        │                   │                   │
     │──────────────────→│                   │                   │
     │                   │                   │                   │
     │                   │ Probe Tier 1-2    │                   │
     │                   │──────────────────→│                   │
     │                   │                   │                   │
     │                   │                   │ Start()           │
     │                   │                   │───┐               │
     │                   │                   │←──┘ 启动          │
     │                   │                   │    ProcessThread   │
     │                   │                   │                   │
     │                   │                   │                   │
  ═══╪═══════════════════╪═══ 数据持续流动 ══╪═══════════════════╪═══
     │                   │                   │                   │
     │                   │            定时器触发 Collect          │
     │                   │                   │                   │
     │                   │                   │ Source.Collect()   │
     │                   │                   │───┐               │
     │                   │                   │←──┘               │
     │                   │                   │                   │
     │                   │                   │ → Process → Sink  │
     │                   │                   │─────────────────→│
     │                   │                   │                   │
     │                   │                   │              SSE push
     │                   │                   │                   │──→ 前端
     │                   │                   │                   │
     │          (用户此时打开浏览器, 数据已在流动)                 │
     │                   │                   │                   │
```

---

## 十、架构设计评价

### 10.1 SOLID 原则对照

| 原则 | 实现方式 | 评价 |
|------|---------|------|
| **S** — 单一职责 | InfrastructureManager 只管资源; FeatureBus 只管编排; FeatureDriver 只管自己的采集逻辑; SseHandler 只管推送 | ✅ 优秀 |
| **O** — 开放-封闭 | 新增 Feature 只需写一个 FeatureDriver 子类 + REGISTER_FEATURE 宏，框架层零修改 | ✅ 优秀 |
| **L** — 里氏替换 | 所有 FeatureDriver 子类通过 FeatureBus 统一调度，基类接口完整定义行为契约 | ✅ 良好 |
| **I** — 接口隔离 | Source/Processor/Sink 各自独立接口; 前端 DataSource 接口支持 Live/Replay 切换 | ✅ 良好 |
| **D** — 依赖倒置 | FeatureBus 依赖 FeatureDriver 抽象; API 路由依赖 FeatureBus 抽象; 前端 Hook 依赖 DataSource 接口 | ✅ 优秀 |

### 10.2 架构亮点

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                            架构核心优势                                           │
│                                                                                 │
│  1. 极简数据流                                                                   │
│     Pipeline → SinkPool → SseSink → SSE → 前端                                 │
│     无 Buffer 中间层, 无 WebSocket 轮询, 端到端 < 100ms                          │
│     代码量: 旧方案 ~1560 行 → 新方案 ~210 行 (减少 87%)                          │
│                                                                                 │
│  2. 零改动扩展                                                                   │
│     新增 Feature: 写一个 .h 文件 (50-70 行) + BUILD 规则                         │
│     前端零改动: 自动发现 + JSON Schema 自动生成配置表单                            │
│     旧方案: 修改 main.cc + FeatureManager + api_routes.h (3 处)                  │
│                                                                                 │
│  3. 线程完全隔离                                                                  │
│     ProcessThread 纯 CPU-bound, 无 I/O                                          │
│     所有 I/O 在池化线程中执行                                                     │
│     慢客户端/慢磁盘不影响数据处理路径                                              │
│                                                                                 │
│  4. 内核态过滤                                                                   │
│     BPF tgid 过滤在内核空间完成                                                  │
│     非目标进程的采样: 零拷贝、零 ring buffer 写入、零用户态开销                     │
│     系统开销从"全系统"降到"仅目标进程"                                             │
│                                                                                 │
│  5. 自描述 + 自动发现                                                            │
│     FeatureDescriptor 声明元数据 + 参数列表                                      │
│     前端通过 GET /api/v2/features 发现所有能力                                   │
│     ConfigSchema() 返回 JSON Schema → 前端自动渲染配置表单                        │
│                                                                                 │
│  6. 优雅停机保证                                                                  │
│     Drain 流程: 排空 channel → 最后一次 Flush → SinkPool 写完                    │
│     数据不丢失: 停机前所有 in-flight 数据都会被处理和写出                          │
│                                                                                 │
│  7. 可测试性                                                                     │
│     每个 FeatureDriver 可独立单元测试                                             │
│     Pipeline 集成测试不依赖 HTTP 服务器                                           │
│     前端 DataSource 接口可 mock 或注入 ReplayEngine                              │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 10.3 架构代码量对比

| 组件 | 旧方案 (行) | 新方案 (行) | 减少 |
|------|------------|------------|------|
| FeatureManager (上帝类) | 1200 | 0 (删除) | -100% |
| PipelineController | 600 | 0 (删除) | -100% |
| StreamSinkStore (Buffer) | 340 | 0 (删除) | -100% |
| WebSocketManager | 600 | 0 (删除) | -100% |
| InfrastructureManager | — | 80 | 新增 |
| FeatureBus | — | 259 | 新增 |
| FeatureDriver 基类 | — | 250 | 新增 |
| SseHandler | — | 150 | 新增 |
| 10× FeatureDriver 子类 | 混在 FeatureManager | 600 | 拆分 |
| **总计** | **~2740** | **~1339** | **-51%** |

---

## 十一、数据模型与序列化

### 11.1 DataBatch 结构

```
DataBatch (C++ 后端)
├── type_: kMetrics | kProfile | kTrace | kLog | kGeneric
├── feature_name_: "cpu_utilization" | "cpu_profiler" | ...
├── timestamp_: uint64_t (epoch ms)
├── seq_: uint64_t (单调递增序号)
├── arena_: shared_ptr<Arena> (内存池, 碰撞指针分配)
├── records_: vector<Record>
│   └── Record:
│       ├── labels: Map<string, string> (维度标签)
│       ├── fields: Map<string, double/string> (指标值)
│       └── timestamp: uint64_t
└── stack_samples_: vector<StackSample>
    └── StackSample:
        ├── comm: string (进程名)
        ├── pid/tid: uint32_t
        ├── kernel_stack: vector<StackFrame>
        ├── user_stack: vector<StackFrame>
        └── count: uint64_t (样本权重)
```

### 11.2 SSE JSON 序列化格式

```json
// time_series 模型 (cpu_utilization):
{
  "feature": "cpu_utilization",
  "modelType": "time_series",
  "timestamp": 1719900000000,
  "seq": 1234,
  "metrics": [
    {
      "labels": {"cpu": "cpu0", "type": "cpu_core"},
      "fields": {"busy_pct": 45.2, "user_pct": 30.1},
      "timestamp": 1719900000000
    }
  ]
}

// profile 模型 (cpu_profiler):
{
  "feature": "cpu_profiler",
  "modelType": "profile",
  "timestamp": 1719900000000,
  "seq": 42,
  "records": [
    {
      "pid": 1234,
      "comm": "nginx",
      "kernel_stack": "do_syscall;__x64_sys_write;vfs_write",
      "user_stack": "main;worker_loop;handle_request",
      "count": 5
    }
  ]
}
```

---

## 十二、配置驱动模型

### 12.1 配置层次

```
illuminator.yaml.example
    │
    ├─ server:
    │   ├─ http.listen: "0.0.0.0:9527"
    │   └─ auth_token: "" (空 = 开发模式, 跳过认证)
    │
    ├─ engine:
    │   ├─ collect_pool_threads: 2
    │   ├─ sink_pool_threads: 4
    │   └─ channel.size: "medium" (4096 slots)
    │
    └─ always_on:
        └─ auto_start_tier_1_2: true
```

### 12.2 Feature 配置 vs 基础设施配置

| 配置类型 | 来源 | 示例 |
|---------|------|------|
| 基础设施 | YAML 文件 (启动时) | 线程池大小, channel 容量, 服务端口 |
| Feature 元数据 | FeatureDriver 代码 (编译时) | Name, Tier, Category, ConfigSchema |
| Feature 运行时参数 | REST API (运行时) | target_pids, frequency_hz, interval_ms |

---

## 十三、错误处理与容错

### 13.1 多层容错机制

```
┌─ 内核层 ────────────────────────────────────────────────────────────┐
│  BPF verifier 保证安全 + ring buffer 满时丢弃旧数据                   │
└──────────────────────────────────────────────────────────────────────┘

┌─ 数据层 ────────────────────────────────────────────────────────────┐
│  AsyncChannel: 队列满 → drop_newest/drop_oldest (可配置)             │
│  SinkPool: pending > 256 → 丢弃 batch (InternalMetrics 记录)        │
│  Aggregator: Flush 保证 swap 语义 (μs 级, 不可能失败)                │
└──────────────────────────────────────────────────────────────────────┘

┌─ 网络层 ────────────────────────────────────────────────────────────┐
│  SSE: 浏览器 EventSource 内置自动重连                                │
│  SseLink: 指数退避 (1s → 30s max)                                   │
│  Keepalive: 每 15s 心跳防止代理/LB 超时                              │
│  分帧: >64KB 自动拆分, 前端重组 (seq + frame_idx + frame_total)      │
└──────────────────────────────────────────────────────────────────────┘

┌─ 前端层 ────────────────────────────────────────────────────────────┐
│  断线: 图表冻结在最后一帧 + "连接中断" 提示                           │
│  重连: 自动恢复 SSE 推送 + 隐藏提示                                  │
│  Feature 启动失败: 显示错误原因 + "重试" 按钮                         │
│  eBPF 不可用: Tier 3 按钮禁用 + tooltip 说明                         │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 十四、性能特征

| 指标 | 目标值 | 实现方式 |
|------|--------|---------|
| 端到端延迟 (采集→渲染) | < 100ms | SSE 直推, 无 Buffer, 条件变量通知 |
| CPU 开销 (Tier 1-2) | < 2% | 高效 procfs 读取 + 1s 间隔 |
| CPU 开销 (Tier 3) | 3-10% | 内核态 tgid 过滤, 49Hz 采样 |
| 内存占用 (后端) | < 200MB | Arena 池化分配 + channel 固定容量 |
| Channel 吞吐 | > 1M msg/s | 无锁 CAS + 三级退避 |
| SseSink 延迟 | < 1μs | 仅内存操作 (push + notify) |
| 前端首屏 | < 500ms | Vite 代码分割 + lazy loading |
| 火焰图渲染 (2000 samples) | < 50ms | Web Worker 异步 + CSS div |
| 前端内存峰值 | < 100MB | ringBuffer 上限 + 组件卸载清理 |

---

## 十五、已实现的 FeatureDriver 列表

| Driver | Name | Tier | 模式 | 数据源 | 自动启动 |
|--------|------|------|------|--------|---------|
| CpuUtilizationDriver | cpu_utilization | Monitoring | Pull | /proc/stat | ✅ |
| ProcessCpuDriver | process_cpu | Monitoring | Pull | /proc/[pid]/stat | ✅ |
| CpuProfilerDriver | cpu_profiler | Profiling | Push (eBPF) | perf_event + BPF | ❌ (手动) |
| OffcpuProfilerDriver | offcpu_profiler | Profiling | Push (eBPF) | sched tracepoint | ❌ (手动) |
| IoMonitorDriver | io_monitor | Tracing | Push (eBPF) | block I/O tracepoint | ✅ |
| NetTracerDriver | net_tracer | Tracing | Push (eBPF) | TCP tracepoint | ✅ |
| SchedAnalyzerDriver | sched_analyzer | Tracing | Push (eBPF) | sched tracepoint | ✅ |

---

## 十六、文件组织与代码导航

```
src/
├── cli/                                CLI 入口 + 序列化
│   ├── main.cc                         daemon 入口 (RunDaemon + RunCollect)
│   └── json_serializer.h              DataBatch → JSON 序列化
├── core/
│   ├── common/                         基础设施（无外部依赖）
│   │   ├── status.h                   StatusCode + Status + StatusOr<T>
│   │   ├── logging.h                  spdlog 封装 + IL_* 日志宏
│   │   ├── config.h                   ConfigValue + PipelineConfig + GlobalConfig
│   │   ├── yaml_config_loader.h       YAML 配置加载 (yaml-cpp)
│   │   ├── data_batch.h              数据模型 (Record + StackSample + Arena)
│   │   ├── string_util.h             通用工具函数
│   │   └── proc_reader.h             /proc 文件系统解析库
│   ├── engine/                        引擎运行时
│   │   ├── infrastructure_manager.h   Layer 1: TimerWheel + CollectPool + SinkPool
│   │   ├── feature_bus.h              Layer 2: Driver 注册与生命周期编排
│   │   ├── feature_driver.h           Layer 3: Driver 基类 (Probe/Remove/BuildPipeline)
│   │   ├── pipeline.h                 Pipeline 类 (AsyncChannel + ProcessThread)
│   │   ├── timer_wheel.h              全局定时调度 (timerfd + epoll + eventfd)
│   │   ├── async_channel.h            无锁通道 (variant<Data, Sentinel>)
│   │   ├── pid_manager.h             PID 管理器 (进程发现 + BPF map 同步)
│   │   └── self_observability.h       自监控 (InternalMetrics + ResourceLimiter)
│   ├── memory/                        内存管理
│   │   ├── arena.h                    碰撞指针内存分配器
│   │   └── lock_free_queue.h          无锁环形队列 (CAS, 2^N 容量)
│   └── threading/                     线程管理
│       ├── thread_pool.h              通用线程池 (生产者-消费者模式)
│       └── thread_util.h              线程命名工具
├── ebpf/                              BPF C 程序 + 加载器
│   ├── include/                       vmlinux.h, event_types.h, bpf_compat.h
│   ├── probes/                        BPF 程序 (cpu/sched/io/net/memory)
│   └── loader/                        BpfProgramManager, FeatureProbe, StackTraceUtil, BpfStatsReader
├── plugin/                            完整插件体系
│   ├── api/                           插件接口定义
│   │   ├── plugin_api.h               Plugin 基类 + C ABI (IlPluginDescriptor)
│   │   ├── source_plugin.h            Source 接口 (Pull/Push 模式)
│   │   ├── processor_plugin.h         Processor 接口
│   │   ├── aggregator_plugin.h        Aggregator 接口
│   │   └── sink_plugin.h              Sink 接口
│   ├── manager/                       插件管理
│   │   ├── plugin_registry.h          集中式工厂注册表 (IL_REGISTER_* 宏)
│   │   ├── plugin_manager.h           .so 插件发现与加载
│   │   ├── so_loader.h                dlopen/dlsym 动态加载
│   │   └── wasm_runtime.h             WASM 沙箱运行时 (预留)
│   ├── builtin/                       内置插件强链接注册
│   │   ├── builtin_plugins.h          声明
│   │   └── builtin_plugins.cc         #include 所有内置插件头文件
│   ├── features/                      FeatureDriver 编排层
│   │   ├── feature_registry.h         REGISTER_FEATURE() 宏 + RegisterAll()
│   │   ├── cpu_utilization_driver.h   典型 Tier 1 Driver 实现
│   │   ├── process_cpu_driver.h
│   │   ├── cpu_profiler_driver.h      Tier 3: eBPF CPU Profiler
│   │   ├── offcpu_profiler_driver.h   Tier 3: eBPF Off-CPU Profiler
│   │   ├── io_monitor_driver.h
│   │   ├── net_tracer_driver.h
│   │   └── sched_analyzer_driver.h
│   ├── sources/                       数据源插件 (按 cpu/sched/io/net 分组)
│   │   ├── ebpf_source_base.h         eBPF 统一基类 (EbpfSourceBase + IL_SKEL_CALLBACKS)
│   │   ├── cpu/                       cpu_utilization, process_cpu, cpu_profiler, proc_stat_reader
│   │   ├── sched/                     sched_analyzer, offcpu_profiler, ebpf_sched_tracer
│   │   ├── io/                        ebpf_io_monitor
│   │   └── net/                       ebpf_net_tracer
│   ├── processors/                    处理器插件
│   │   ├── passthrough/               透传处理器 (测试用)
│   │   ├── filter/                    标签过滤处理器
│   │   ├── stack_symbolizer/          堆栈符号化 (ELF + kallsyms + demangle)
│   │   └── stack_merger/              堆栈合并器
│   ├── aggregators/                   聚合器插件
│   │   └── cpu_stats_aggregator/      CPU 统计时间窗口聚合
│   └── sinks/                         输出插件
│       ├── console_output/            控制台输出 (文本/JSON)
│       ├── file_export/               JSONL 文件导出
│       ├── local_storage/             SQLite 本地存储
│       ├── pprof_export/              pprof 折叠栈格式导出
│       ├── prometheus_exposition/     Prometheus 指标暴露
│       ├── otlp_export/               OTLP JSON over HTTP
│       ├── recording_sink/            按需录制 (.ilr NDJSON)
│       └── fanout/                    零拷贝多路分发
└── server/                            服务层
    ├── http_server.h                  cpp-httplib 封装
    ├── api_routes.h                   REST API 路由 (控制面)
    ├── sse_handler.h                  SSE 订阅管理 + SseSink 推送 (数据面)
    └── storage/                       存储后端
        ├── storage_backend.h          抽象接口 + 工厂
        └── sqlite_backend/            SQLite 实现 (WAL 模式)

web/src/
├── services/
│   ├── dataBus.ts                     SSE 数据总线 (全局单例)
│   ├── sseLink.ts                     EventSource 封装 (自动重连)
│   ├── apiClient.ts                   REST API 客户端
│   └── dataSource.ts                  DataSource 接口定义
├── hooks/
│   ├── useDataSource.ts               getDataSource() 全局单例
│   ├── useCpuData.ts                  CPU 数据 Hook
│   └── ...                            其他数据 Hook
├── components/charts/
│   ├── ProfileSnapshot.tsx            火焰图 (SSE 订阅 + Worker)
│   └── EChart.tsx                     ECharts 包装器
├── workers/
│   └── flameGraphWorker.ts            火焰图树构建 (Web Worker)
└── pages/                             10 个懒加载页面
```

---

## 附录 A: 关键设计决策记录

| 决策点 | 选择 | 被拒绝的方案 | 理由 |
|--------|------|-------------|------|
| 实时推送 | SSE (Server-Sent Events) | WebSocket | 单向推送场景, 浏览器原生支持, 自动重连, 代理兼容 |
| Buffer | 无 (直推) | StreamSinkStore (340行) | 实时推送/重连/录制均不需要 buffer |
| PID 过滤 | `bpf_get_ns_current_pid_tgid()` BPF 内核态过滤 | 用户态 comm 学习 + 升级 | 内核 helper 零开销实现 namespace 透明，BPF 侧直接用 ns-local PID 过滤 |
| Feature 管理 | 去中心化 FeatureDriver | 中心化 FeatureManager | 消除上帝类, 每个 Feature 自包含 |
| 定时器 | timerfd + epoll + eventfd | condition_variable + priority_queue | 更高精度, 支持外部唤醒 |
| Channel 出队 | 三级自适应退避 | 简单阻塞 | 高频时低延迟(spin), 低频时省 CPU(sleep) |
| Pipeline 拥有者 | FeatureDriver (成员变量) | PipelineController 集中管理 | 与 Linux Driver 模型一致, 资源自管理 |
| 前端数据接口 | DataSource 抽象接口 | 直接调用 API | 支持 Live/Replay 无缝切换 |
| SSE 连接模型 | 订阅制 (subscribe + update) | 全局流 (固定 /events) | 按需过滤, 减少无用数据传输 |
| 大消息处理 | 自动分帧 (64KB 阈值) | 限制消息大小 | 火焰图 payload 可能很大 |

---

## 附录 B: 与常见可观测工具架构对比

| 维度 | Illuminator | Prometheus | Grafana Agent | Vector |
|------|-------------|-----------|---------------|--------|
| 采集模型 | Push (eBPF) + Pull (procfs) | Pull only | Push + Pull | Push + Pull |
| 数据传输 | SSE (直推) | HTTP Scrape | gRPC/HTTP | TCP/HTTP |
| 管道架构 | Per-Feature Pipeline | 无管道 | Component DAG | Topology DAG |
| 驱动模型 | Linux Driver 风格 | 无 | Component 注册 | Transform 链 |
| 内核集成 | libbpf CO-RE (深度) | 无 | eBPF (浅) | 无 |
| 前端 | 自带 React SPA | 无 (Grafana) | 无 | 无 |
| 部署模型 | 单二进制 (嵌入式友好) | 单二进制 | Agent + Cloud | Agent 模式 |
| 主要场景 | 车端/嵌入式性能分析 | 通用监控 | 数据转发 | 数据管道 |

---

## 附录 C: PID Namespace 透明过滤

### 问题

容器或 PID namespace 环境下，BPF 的 bpf_get_current_pid_tgid() 返回 root namespace PID，与用户态 getpid() 的 namespace-local PID 不一致，导致 BPF PID 过滤失败。

### 方案

使用 Linux 5.7+ 的 bpf_get_ns_current_pid_tgid(dev, ino) helper：

1. 用户态 stat("/proc/self/ns/pid") 获取 PID namespace 的 dev/ino
2. 写入 BPF map (cpu_pidns_cfg / offcpu_pidns_cfg)
3. BPF 调用 helper 将 root-ns PID 翻译为 namespace-local PID
4. 直接用 namespace-local PID 过滤，零启动延迟

### 优势

- 启动即生效（无学习期）
- BPF 内核态过滤（零用户态开销）
- PID 精确匹配（无 comm 碰撞风险）
- 代码简洁（约 20 行替代原 100+ 行 comm 学习机制）

### 代码路径

- event_types.h: struct il_pidns_config
- cpu_profiler.bpf.c / offcpu_profiler.bpf.c: pidns_cfg map + ns helper
- cpu_profiler.h / offcpu_profiler.h: ConfigurePidNamespace()
