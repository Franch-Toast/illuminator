# Illuminator

**高性能、插件化的全栈可观测性与深度性能分析平台**

基于 C++ 实现，利用 eBPF 技术在 Linux 系统上进行零侵入数据采集，覆盖 CPU、内存、网络、磁盘 I/O、调度器等多个领域，配套自研 Web 可视化平台并兼容业界标准格式导出。

---

## 核心特性

- **eBPF 零侵入采集**：基于 libbpf + CO-RE 的现代 eBPF 模式，支持 CPU 性能剖析、内存分配追踪、网络连接监控、块 I/O 延迟分析、调度事件追踪
- **事件驱动异步管道 (v3)**：`Source → AsyncChannel → Processor → Aggregator → Sink` 四阶段管道，基于 TimerWheel (timerfd+epoll) 统一调度、CollectPool 并行采集、ProcessThread 纯事件处理、SinkPool I/O 隔离，支持 Pull/Push 双模式、FlushSentinel 信号机制、水位线反压
- **三层插件系统**：
  - **Builtin**（内建）：编译时链接，零开销
  - **Shared Object**（动态库）：运行时 `.so` 加载，稳定 C ABI
  - **WASM**（沙箱）：多语言编写，内存隔离
- **存储抽象层**：可插拔后端（SQLite 默认）
- **多格式导出**：pprof、OTLP、Prometheus、JSON
- **Web 可视化平台**：实时仪表盘、火焰图（On-CPU/Off-CPU）、调度器时间线、对比分析
- **WebSocket 实时推送**：独立端口（默认 9528），前端自动重连
- **自观测能力**：内部指标（Counter/Gauge/Histogram）、健康检查、RSS 资源限制器

---

## 架构概览

```
                    ┌───────────────────────────────────┐
                    │       Illuminator 总体架构         │
                    └───────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────────┐
  │                    Configuration Layer                          │
  │  YAML Pipeline Config  │  Plugin Manifest  │  CLI / API        │
  └──────────────────────┬─────────────────────────────────────────┘
                         │
  ┌──────────────────────▼─────────────────────────────────────────┐
  │              Core Engine (C++17) — Pipeline v3 事件驱动架构      │
  │                                                                  │
  │  ┌──────────────────────────────────────────────────────────┐   │
  │  │ TimerWheel (timerfd+epoll, 1 thread)                     │   │
  │  │  定时触发 Collect 事件 + FlushSentinel 注入               │   │
  │  └──────────────────┬───────────────┬───────────────────────┘   │
  │          ┌──────────▼──────┐  ┌─────▼─────────────────────┐    │
  │          │ CollectPool     │  │ Per Pipeline:              │    │
  │          │ (M threads)     │  │  AsyncChannel<variant>     │    │
  │          │ src.Collect()   │──│  → ProcessThread (1/pipe)  │    │
  │          └─────────────────┘  │    match: Data → Process   │    │
  │   Push: eBPF callback ───────→│    match: Sentinel → Flush │    │
  │                               │  → SinkPool (K threads)    │    │
  │                               └────────────────────────────┘    │
  │                                                                  │
  │  Shared Infra: Arena + LockFreeQueue + ThreadPool                │
  │  eBPF Subsystem: libbpf Loader + Ring Buffer + BTF Cache         │
  │  Plugin Manager: SO Loader + WASM Runtime + Plugin Registry      │
  │  Storage Layer: SQLite (WAL 模式)                                │
  │  Export Layer: pprof / OTLP / Prometheus / JSON                  │
  │  Self-Observability: InternalMetrics / ResourceLimiter / /metrics│
  └──────────────────────┬─────────────────────────────────────────┘
                         │ HTTP (cpp-httplib) / WebSocket (独立端口)
  ┌──────────────────────▼─────────────────────────────────────────┐
  │              Web 可视化平台 (React + TypeScript)                 │
  │  Dashboard │ Flame Graph │ Timeline │ Diff View │ Query Console │
  └────────────────────────────────────────────────────────────────┘
```

### 线程模型 (N 管道)

| 线程 | 数量 | 命名 | 职责 |
|------|------|------|------|
| TimerWheel | 1 | `timer-wheel` | timerfd+epoll 事件调度，不执行实际工作 |
| CollectPool | M (默认 2) | `collect-N` | 并行执行 Source::Collect() I/O |
| ProcessThread | N (每管道 1) | `{pipeline_name}` | 纯事件处理器 — variant dispatch |
| SinkPool | K (默认 4) | `sink-write-N` | 并行执行 Sink::Write() I/O |
| HTTP | 1 | `http-server` | REST API + 静态文件 |
| WebSocket | 2 | `ws-broadcast` / `ws-accept` | 实时数据推送 / 连接监听 |
| eBPF 轮询 | 按需 | `profiler-poll` / `sched-poll` 等 | Push Source ring buffer 轮询 |

---

## 项目结构

```
illuminator/
├── MODULE.bazel                # Bazel bzlmod 依赖管理
├── .bazelrc                    # Bazel 编译配置（C++17, sanitizers 等）
├── illuminator.yaml.example    # 全功能配置文件 (唯一标准配置)
├── README.md
│
├── docs/                       # ===== 设计文档 =====
│   └── pipeline_v3_design.md   #   Pipeline v3 事件驱动架构设计文档
│
├── src/                        # ===== 全部 C++ 源代码 =====
│   ├── core/                   # 核心引擎
│   │   ├── common/             #   Status, Logger(spdlog), Config, StringUtil, SelfObservability
│   │   ├── config/             #   YAML 配置加载器 (yaml-cpp)
│   │   ├── engine/             #   PipelineController, AsyncChannel, TimerWheel, DataBatch
│   │   ├── memory/             #   Arena(零拷贝), LockFreeQueue(MPSC 无锁环形缓冲)
│   │   └── threading/          #   ThreadPool, ThreadUtil(线程命名)
│   │
│   ├── plugin/                 # 插件框架
│   │   ├── api/                #   插件接口 (Source/Processor/Aggregator/Sink) + C ABI
│   │   ├── manager/            #   PluginRegistry + SO Loader + WASM Runtime
│   │   └── builtin/            #   内建插件强链接清单
│   │
│   ├── ebpf/                   # eBPF 子系统
│   │   ├── include/            #   vmlinux.h, event_types.h, bpf_compat.h
│   │   ├── probes/             #   BPF 探针源码 (按子系统分类)
│   │   │   ├── cpu/            #     cpu_profiler.bpf.c, cpu_sampler.bpf.c
│   │   │   ├── sched/          #     sched_analyzer.bpf.c, sched_tracer.bpf.c, offcpu_profiler.bpf.c
│   │   │   ├── io/             #     bio_latency.bpf.c
│   │   │   ├── net/            #     net_tracer.bpf.c
│   │   │   └── memory/         #     mem_tracer.bpf.c
│   │   └── loader/             #   BpfProgramManager, FeatureProbe, StackTraceUtil
│   │
│   ├── sources/                # Source 插件 (按子系统分类)
│   │   ├── cpu/                #   4 个 CPU 相关源
│   │   │   ├── cpu_profiler/   #     eBPF perf_event CPU 性能剖析 (Push)
│   │   │   ├── cpu_utilization/#     CPU 利用率 /proc/stat (Pull, EMA 平滑)
│   │   │   ├── process_cpu/    #     进程/线程 CPU 监控 (Pull, Top-N)
│   │   │   └── proc_stat_reader/#    /proc/stat 底层读取器
│   │   ├── sched/              #   3 个调度器相关源
│   │   │   ├── sched_analyzer/ #     调度分析 (eBPF, 运行队列延迟, 迁移追踪)
│   │   │   ├── ebpf_sched_tracer/#   eBPF 调度事件追踪
│   │   │   └── offcpu_profiler/#     Off-CPU 性能剖析 (eBPF)
│   │   ├── io/                 #   eBPF 块 I/O 延迟监控
│   │   │   └── ebpf_io_monitor/
│   │   └── net/                #   eBPF TCP 连接追踪
│   │       └── ebpf_net_tracer/
│   │
│   ├── processors/             # Processor 插件
│   │   ├── passthrough/        #   透传处理器 (测试用)
│   │   ├── filter/             #   标签过滤处理器
│   │   ├── stack_symbolizer/   #   堆栈符号化 (ELF + kallsyms + C++ demangle)
│   │   └── stack_merger/       #   相同调用栈合并
│   │
│   ├── aggregators/            # Aggregator 插件
│   │   └── cpu_stats_aggregator/#  CPU 统计聚合 (时间窗口)
│   │
│   ├── sinks/                  # Sink 插件 (7 个)
│   │   ├── console_output/     #   控制台输出 (文本/JSON)
│   │   ├── file_export/        #   JSONL 文件导出
│   │   ├── local_storage/      #   SQLite 存储后端写入
│   │   ├── pprof_export/       #   pprof 折叠栈格式 (兼容 FlameGraph)
│   │   ├── prometheus_exposition/#  Prometheus 指标暴露
│   │   ├── otlp_export/        #   OpenTelemetry OTLP (JSON over HTTP)
│   │   └── websocket_sink/     #   WebSocket 实时推送
│   │
│   ├── serialization/          # JSON 序列化 (nlohmann/json)
│   │   └── json_serializer.h   #   DataBatch → JSON
│   │
│   ├── storage/                # 存储抽象层
│   │   ├── storage_backend.h   #   StorageBackend 接口 + StorageFactory
│   │   └── sqlite_backend/     #   SQLite 实现 (WAL 模式)
│   │
│   ├── server/                 # HTTP / WebSocket 服务
│   │   ├── http_server.h       #   cpp-httplib 薄封装
│   │   ├── api_routes.h        #   REST API 路由注册 (从 main.cc 拆出)
│   │   ├── websocket_server.h  #   WebSocket 协议实现 (内联 SHA-1)
│   │   └── websocket_manager.h #   WebSocket 连接管理 + 独立监听端口
│   │
│   └── cli/                    # 命令行入口
│       └── main.cc             #   daemon / collect / top / plugins / version
│
├── third_party/                # ===== 第三方库 (vendored) =====
│   └── cpp-httplib/            #   cpp-httplib (单头文件 HTTP 服务器)
│
└── web/                        # ===== Web 前端 (React + TypeScript) =====
    ├── package.json            #   npm 依赖
    ├── vite.config.ts          #   Vite 构建配置 (含 API 代理)
    ├── src/
    │   ├── App.tsx             #   路由: / /processes /flamegraph /timeline /diff /query
    │   ├── hooks/
    │   │   ├── useApi.ts       #     REST API hooks (pipelines, CPU, sched 等)
    │   │   ├── useCpuMetrics.ts#     CPU 指标专用 hook
    │   │   └── useWebSocket.ts #     WebSocket 自动重连 hook
    │   ├── styles/
    │   │   └── theme.ts        #     设计令牌 (颜色、间距、字体)
    │   └── pages/              #   6 个页面组件
    │       ├── CpuOverview.tsx  #     CPU 概览 (利用率/核心热力图/Pipeline Channels)
    │       ├── ProcessExplorer.tsx#   进程/线程浏览器 (Top-N, 排序/过滤)
    │       ├── FlameGraph.tsx   #     火焰图 (On-CPU / Off-CPU, SVG 导出)
    │       ├── Timeline.tsx     #     调度器分析 (时序/迁移/Wakeup/延迟分布)
    │       ├── DiffView.tsx     #     Profile 对比分析
    │       └── QueryConsole.tsx #     SQL 查询控制台 (WIP)
    └── dist/                   #   前端构建产物
```

---

## 依赖

### 系统依赖

| 依赖 | 最低版本 | 用途 |
|------|---------|------|
| **Linux 内核** | 5.8+ | eBPF Ring Buffer 支持 |
| **Bazel** | 7.0+ | 构建系统 (推荐使用 Bazelisk) |
| **Clang** | 14+ | BPF 探针编译 (`-target bpf`) |
| **libbpf** | 1.0+ | eBPF 程序加载器 |
| **libelf + zlib** | - | ELF 解析 (libbpf 依赖) |
| **SQLite3** | 3.35+ | 默认存储后端 |
| **Node.js** | 18+ | Web 前端构建 (可选) |

### Bazel 管理的 C++ 依赖 (自动下载)

| 库 | 版本 | 用途 |
|----|------|------|
| spdlog | 1.14.1 | 结构化日志 (原生 fmt 风格) |
| nlohmann/json | 3.11.3 | JSON 序列化 |
| yaml-cpp | 0.8.0 | YAML 配置解析 |

### Web 前端依赖 (npm)

| 库 | 用途 |
|----|------|
| React 18 | UI 框架 |
| react-router-dom | 路由 |
| Recharts | 图表组件 |
| d3-flame-graph | 火焰图渲染 |
| Vite 5 | 构建工具 |

---

## 构建

### 安装系统依赖 (Ubuntu/Debian)

```bash
# Bazel (推荐使用 Bazelisk)
sudo apt install -y npm && sudo npm install -g @bazel/bazelisk

# eBPF 工具链
sudo apt install -y clang llvm libbpf-dev libelf-dev zlib1g-dev

# SQLite
sudo apt install -y libsqlite3-dev
```

### 构建 C++ 后端

```bash
# 标准构建
bazel build //src/cli:illuminator

# 优化构建
bazel build //src/cli:illuminator --config=opt

# Debug 构建
bazel build //src/cli:illuminator --config=dbg
```

### 编译 BPF 探针

BPF 探针可通过 Bazel 编译（推荐）：

```bash
# 编译全部 8 个 BPF 探针
bazel build //src/ebpf/probes:all

# 编译单个探针
bazel build //src/ebpf/probes:cpu_profiler
```

### 构建 Web 前端 (可选)

```bash
cd web
npm install
npm run build
```

构建产物输出到 `web/dist/`，会被 HTTP 服务器自动托管。

---

## 使用方法

### 守护进程模式

启动 Illuminator 守护进程，开启 HTTP 服务（默认端口 9527）和 WebSocket 实时推送（默认端口 9528）：

```bash
sudo ./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example
```

> 注意：eBPF 数据采集需要 root 权限。

服务启动后可访问：

**Web 界面**
- `http://localhost:9527` — 可视化仪表盘（CPU 概览 / 进程 / 火焰图 / 调度器）

**REST API**
- `GET /healthz` — 健康检查
- `GET /metrics` — Prometheus exposition 格式指标
- `GET /api/v1/pipelines` — 管道状态（含 channel 统计）
- `GET /api/v1/pipelines/:name/collect` — 触发指定管道一次性采集
- `GET /api/v1/channel_stats` — 所有管道 channel 详细统计
- `GET /api/v1/cpu/utilization` — CPU 利用率
- `GET /api/v1/cpu/processes` — 进程 CPU 指标
- `GET /api/v1/cpu/profile/flamegraph` — On-CPU 火焰图
- `GET /api/v1/cpu/profile/offcpu` — Off-CPU 火焰图
- `GET /api/v1/cpu/sched/summary` — 调度器摘要
- `GET /api/v1/cpu/sched/history` — 调度历史
- `GET /api/v1/cpu/sched/events` — 调度事件
- `GET /api/v1/cpu/sched/wakeups` — Wakeup 链
- `GET /api/v1/internal_metrics` — 内部指标（JSON）

**WebSocket**
- `ws://localhost:9528/ws/<pipeline>` — 实时数据推送

### 一次性采集

```bash
sudo ./bazel-bin/src/cli/illuminator collect --duration 30
```

### 实时系统概览 (类 top)

```bash
sudo ./bazel-bin/src/cli/illuminator top
```

### 其他命令

```bash
# 查看版本
./bazel-bin/src/cli/illuminator version

# 列出已注册的插件
./bazel-bin/src/cli/illuminator plugins

# 列出可用存储后端
./bazel-bin/src/cli/illuminator storage
```

---

## 配置

参见 [illuminator.yaml.example](illuminator.yaml.example) 获取完整配置参考。核心配置结构：

```yaml
global:
  log_level: info
  data_dir: /var/lib/illuminator
  plugin_dirs:
    - /etc/illuminator/plugins

server:
  http:
    enabled: true
    listen: "0.0.0.0:9527"
  websocket:
    enabled: true

engine:
  collect_pool_threads: 2     # CollectPool 线程数 (0=auto)
  sink_pool_threads: 4        # SinkPool 线程数 (0=auto)
  channel:
    size: medium              # small(1024) | medium(4096) | large(16384)
    drop_policy: drop_newest  # drop_newest | drop_oldest
    backpressure_high: 0.8    # 触发反压水位线
    backpressure_low: 0.2     # 解除反压水位线

pipelines:
  cpu_profiling:
    source:
      type: cpu_profiler
      config:
        frequency_hz: 49
        bpf_object: bazel-bin/src/ebpf/probes/cpu_profiler.bpf.o
    processors:
      - type: stack_symbolizer
      - type: stack_merger
    aggregator:
      type: cpu_stats_aggregator
      config:
        window_sec: 30
    sinks:
      - type: local_storage
      - type: websocket_sink
```

---

## 插件开发

### 内建插件

实现对应接口并使用宏注册：

```cpp
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class MySource : public SourcePlugin {
public:
    const char* Name() const override { return "my_source"; }
    const char* Version() const override { return "0.1.0"; }
    StatusOr<DataBatchPtr> Collect() override { /* ... */ }
};

IL_REGISTER_SOURCE("my_source", MySource);

}  // namespace illuminator
```

### 动态库插件 (.so)

导出稳定 C ABI 接口：

```cpp
#include "plugin/api/plugin_api.h"

extern "C" const IlPluginDescriptor* illuminator_plugin_describe() {
    static IlPluginDescriptor desc = {
        .name = "my_plugin",
        .version = "1.0.0",
        .type = 0,  // SOURCE
        .create = my_create,
        .process = my_process,
        .destroy = my_destroy,
    };
    return &desc;
}
```

---

## 许可证

MIT License
