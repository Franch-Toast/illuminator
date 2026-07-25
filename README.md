# Illuminator

**高性能、插件化的全栈可观测性与深度性能分析平台**

基于 C++ 实现，利用 eBPF 技术在 Linux 系统上进行零侵入数据采集，覆盖 CPU、内存、网络、磁盘 I/O、调度器等多个领域，配套自研 Web 可视化平台并兼容业界标准格式导出。

---

## 核心特性

- **eBPF 零侵入采集**：基于 libbpf + CO-RE + bpftool skeleton 的现代 eBPF 模式（BPF 字节码嵌入二进制），支持 CPU 性能剖析、内存分配追踪、网络连接监控、块 I/O 延迟分析、调度事件追踪
- **事件驱动异步管道 (v3)**：`Source → AsyncChannel → Processor → Aggregator → Sink` 四阶段管道，基于 TimerWheel (timerfd+epoll) 统一调度、CollectPool 并行采集、ProcessThread 纯事件处理、SinkPool I/O 隔离，支持 Pull/Push 双模式、FlushSentinel 信号机制、水位线反压
- **三层插件系统**：
  - **Builtin**（内建）：编译时链接，零开销
  - **Shared Object**（动态库）：运行时 `.so` 加载，稳定 C ABI
  - **WASM**（沙箱）：多语言编写，内存隔离
- **存储抽象层**：可插拔后端（SQLite 默认）
- **多格式导出**：pprof、OTLP、Prometheus、JSON
- **Web 可视化平台**：实时仪表盘、火焰图（On-CPU/Off-CPU）、调度器分析、差异火焰图、SQL 查询控制台、系统健康监控
  - **全局时间控制**：LIVE/PAUSED 模式切换、30s/1m/5m/15m 时间窗口、键盘快捷键（Space/T/?）
  - **Zustand 状态管理**：全局时间、管道状态、过滤器三大 Store
  - **TimeSeriesStore**：前端 RingBuffer 时间序列缓存，支持按时间范围查询和订阅通知
- **SSE 实时推送**：基于 Server-Sent Events 的订阅式数据推送（HTTP 端口 9527），支持 64KB 帧分割、Last-Event-ID 重放、15s 心跳、动态订阅更新
- **动态 Feature 管理（热插拔）**：用户可通过 API/前端实时启动/停止功能模块，无需重启。内置 `SseSink`（SSE 实时推送）、`RecordingSink`（按需录制为 `.ilr` NDJSON 文件，支持大小限制）
- **深度堆栈符号解析**：合并 `.symtab` + `.dynsym`、build-id 调试信息查找、nearest-symbol 启发式（gap 归属）、PID 命名空间感知、`[vdso]` 处理、C++ 自动 demangle，综合解析率 97%+
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
  │          │ (M threads)     │  │  AsyncChannel(variant)     │    │
  │          │ src.Collect()   │──│  → ProcessThread (1/pipe)  │    │
  │          └─────────────────┘  │    match: Data → Process   │    │
  │   Push: eBPF callback ───────→│    match: Sentinel → Flush │    │
  │                               │  → SinkPool (K threads)    │    │
  │                               └────────────────────────────┘    │
  │                                                                  │
  │  Shared Infra: Arena + LockFreeQueue + ThreadPool                │
  │  eBPF Subsystem: bpftool gen skeleton (嵌入字节码) + Ring Buffer + BTF │
  │  Plugin Manager: SO Loader + WASM Runtime + Plugin Registry      │
  │  Storage Layer: SQLite (WAL 模式)                                │
  │  Export Layer: pprof / OTLP / Prometheus / JSON                  │
  │  Self-Observability: InternalMetrics / ResourceLimiter / /metrics│
  └──────────────────────┬─────────────────────────────────────────┘
                         │ HTTP + SSE (cpp-httplib, port 9527)
  ┌──────────────────────▼─────────────────────────────────────────┐
  │              Web 可视化平台 (React + TypeScript + Zustand)         │
  │  TimeControls │ Overview │ CPU │ Memory │ IO │ Network │ GPU    │
  │  Query Console │ Plugins │ System │ Replay │ Keyboard Shortcuts  │
  └────────────────────────────────────────────────────────────────┘
```

### 线程模型 (N 管道)

| 线程 | 数量 | 命名 | 职责 |
|------|------|------|------|
| TimerWheel | 1 | `timer-wheel` | timerfd+epoll 事件调度，不执行实际工作 |
| CollectPool | M (默认 2) | `collect-N` | 并行执行 Source::Collect() I/O |
| ProcessThread | N (每管道 1) | `{pipeline_name}` | 纯事件处理器 — variant dispatch |
| SinkPool | K (默认 4) | `sink-write-N` | 并行执行 Sink::Write() I/O |
| HTTP | 1 | `http-server` | REST API + SSE 数据面 + 静态文件 |
| eBPF 轮询 | 按需 | `profiler-poll` / `sched-poll` 等 | Push Source ring buffer 轮询 |

---

## 项目结构

```
illuminator/
├── MODULE.bazel                # Bazel bzlmod 依赖管理
├── .bazelrc                    # Bazel 编译配置（C++20, stamp, sanitizers）
├── .bazelversion               # Bazel 版本锁定 (7.6.1)
├── Makefile                    # 常用命令快捷封装
├── Dockerfile                  # 多阶段 Docker 构建（前端+后端+runtime）
├── illuminator.yaml.example    # 全功能配置文件 (唯一标准配置)
├── LICENSE                     # MIT License
├── README.md
│
├── tools/                      # ===== 构建工具 =====
│   ├── workspace_status.sh     #   Bazel stamp: 输出 git 版本信息
│   ├── version.bzl             #   Starlark: 生成 version_generated.h
│   └── BUILD
│
├── scripts/                    # ===== 脚本 =====
│   └── check_env.sh           #   环境检测（编译+运行环境）
│
├── docs/                       # ===== 设计文档 =====
│   ├── architecture_overview.md #  系统架构全景分析 v2.0
│   ├── pipeline_v3_design.md   #   Pipeline v3 事件驱动架构设计文档
│   ├── ebpf_plugin_redesign.md #   eBPF 插件基类重设计 RFC
│   ├── architecture_audit_report.md # 全面架构审计报告
│   ├── frontend_architecture_design.md # 前端架构设计 v3.1
│   └── onboarding_guide.md     #   新人入门指南
│
├── src/                        # ===== 全部 C++ 源代码 =====
│   ├── core/                   # 核心引擎
│   │   ├── common/             #   Status, Logger(spdlog), Config, StringUtil, SelfObservability
│   │   ├── config/             #   YAML 配置加载器 (yaml-cpp)
│   │   ├── engine/             #   PipelineController, AsyncChannel, TimerWheel, DataBatch
│   │   ├── memory/             #   Arena(零拷贝+OOM 回调), LockFreeQueue(MPSC 无锁环形缓冲,运行时容量)
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
│   │   └── loader/             #   FeatureProbe, StackTraceUtil
│   │
│   ├── plugin/                 # 插件框架 + 所有插件
│   │   ├── api/                #   插件接口 (Source/Processor/Aggregator/Sink) + C ABI
│   │   ├── manager/            #   PluginRegistry + SO Loader
│   │   ├── builtin/            #   内建插件强链接清单
│   │   ├── features/           #   FeatureDriver 实现 + FeatureRegistry
│   │   ├── sources/            #   Source 插件 (按子系统分类)
│   │   │   ├── ebpf_skeleton_source.h     # eBPF Push Source 基类模板
│   │   │   ├── ebpf_skeleton_pull_source.h# eBPF Pull Source 基类模板
│   │   │   ├── cpu/            #   cpu_profiler, cpu_utilization, process_cpu, proc_stat_reader
│   │   │   ├── sched/          #   sched_analyzer, ebpf_sched_tracer, offcpu_profiler
│   │   │   ├── io/             #   ebpf_io_monitor
│   │   │   └── net/            #   ebpf_net_tracer
│   │   ├── processors/         #   passthrough, filter, stack_symbolizer, stack_merger
│   │   ├── aggregators/        #   cpu_stats_aggregator
│   │   └── sinks/              #   console, file, local_storage, pprof, prometheus, otlp, recording
│   │
│   ├── server/                 # HTTP/SSE 服务
│   │   ├── http_server.h       #   cpp-httplib 封装
│   │   ├── api_routes.h        #   REST API 路由
│   │   ├── sse_handler.h       #   SSE 实时推送
│   │   └── storage/            #   存储抽象层
│   │       ├── storage_backend.h #   StorageBackend 接口 + StorageFactory
│   │   └── sqlite_backend/     #   SQLite 实现 (WAL 模式)
│   │
│   ├── server/                 # HTTP + SSE 服务（见上方 server/ 部分）
│   │
│   └── cli/                    # 命令行入口
│       └── main.cc             #   daemon / collect / top / plugins / version
│
├── third_party/                # ===== 第三方库 (vendored) =====
│   └── cpp-httplib/            #   cpp-httplib (单头文件 HTTP 服务器)
│
└── web/                        # ===== Web 前端 (React + TypeScript + Zustand) =====
    ├── package.json            #   npm 依赖 (含 zustand, date-fns)
    ├── vite.config.ts          #   Vite 构建配置 (含 API 代理)
    ├── src/
    │   ├── App.tsx             #   路由 + 全局键盘快捷键 (Space/T/?) + 快捷键帮助
    │   ├── stores/             #   Zustand 全局状态管理
    │   │   ├── useTimeStore.ts #     全局时间 (LIVE/PAUSED, 时间窗口, 游标)
    │   │   ├── usePipelineStore.ts#  管道状态
    │   │   └── useFilterStore.ts#    全局过滤器 (PID, comm, CPU)
    │   ├── services/           #   数据服务层
    │   │   ├── apiClient.ts    #     统一 REST API 客户端 (类型安全)
    │   │   ├── sseLink.ts     #     SSE 连接管理器 (自动重连 + 心跳)
    │   │   ├── dataBus.ts     #     SSE 数据总线 (批量缓冲 + 背压)
    │   │   ├── dataSource.ts  #     DataSource 抽象 (Live/Replay)
    │   │   └── timeSeriesStore.ts#   前端 RingBuffer 时间序列缓存
    │   ├── components/         #   共享 UI 组件
    │   │   ├── TimeControls/   #     全局时间控制器 (LIVE/PAUSED, 窗口选择)
    │   │   └── Layout/         #     StatusBar (管道状态, WS 连接)
    │   ├── hooks/              #   自定义 hooks
    │   │   ├── usePolling.ts   #     通用轮询 hook (响应全局时间模式)
    │   │   ├── usePipelinePolling.ts# 管道状态轮询
    │   │   ├── useCpuData.ts   #     CPU 指标专用 hook
    │   │   └── useFeatureStream.ts #  Feature 数据流 hook
    │   ├── styles/
    │   │   └── theme.ts        #     设计令牌 (颜色、间距、字体)
    │   └── pages/              #   7 个页面组件
    │       ├── CpuOverview.tsx  #     Dashboard (利用率/热力图/Core Timeline/Pipeline Health)
    │       ├── ProcessExplorer.tsx#   进程/线程浏览器 (Top-N, 排序/过滤)
    │       ├── FlameGraph.tsx   #     Profiler (On-CPU/Off-CPU, 快照缓存, SVG 导出)
    │       ├── Timeline.tsx     #     Scheduler (Overview/TimeSeries/Gantt/Wakeups)
    │       ├── DiffView.tsx     #     Compare (双 Profile 捕获, 差异火焰图, Diff 表格)
    │       ├── QueryConsole.tsx #     SQL 查询控制台
    │       └── SystemPage.tsx   #     System (健康状态, 管道详情, 内部指标)
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
| **bpftool** | 7.0+ | BPF skeleton 头文件生成 (`bpftool gen skeleton`) |
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
| d3-flame-graph | 火焰图渲染（含差异火焰图） |
| Zustand | 全局状态管理 |
| date-fns | 日期格式化工具 |
| Vite 5 | 构建工具 |

---

## 快速开始

```bash
# 检测本地环境是否满足构建/运行要求
make check-env

# 编译后端 + BPF 探针
make build && make probes

# 启动开发模式
make dev
```

---

## 构建

### 安装系统依赖 (Ubuntu/Debian)

```bash
# Bazel (推荐使用 Bazelisk)
sudo apt install -y npm && sudo npm install -g @bazel/bazelisk

# eBPF 工具链
sudo apt install -y clang llvm libbpf-dev libelf-dev zlib1g-dev \
  linux-tools-common linux-tools-generic

# SQLite
sudo apt install -y libsqlite3-dev
```

> 使用 `make check-env` 或 `bash scripts/check_env.sh --all` 可自动检测环境并给出修复建议。

### Makefile 快捷命令

| 命令 | 说明 |
|------|------|
| `make build` | 标准构建（debug） |
| `make build-opt` | 优化构建 |
| `make test` | 运行全部单元测试 |
| `make probes` | 编译 eBPF 探针 |
| `make dev` | 启动后端守护进程 |
| `make dev-web` | 启动前端开发服务器 |
| `make docker` | 构建 Docker 镜像（自动注入版本） |
| `make asan` | AddressSanitizer 测试 |
| `make tsan` | ThreadSanitizer 测试 |
| `make version` | 显示当前版本 |
| `make check-env` | 检测编译/运行环境 |

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

BPF 探针通过 Bazel 编译，使用 `bpftool gen skeleton` 生成类型安全的骨架头文件（`.skel.h`），BPF 字节码嵌入二进制无需外部 `.bpf.o` 文件：

```bash
# 编译全部 BPF 探针（含 skeleton 生成）
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

### Docker 构建

```bash
# 使用 Makefile（自动注入 git 版本）
make docker

# 手动构建
docker build \
  --build-arg GIT_VERSION=$(git describe --tags --always) \
  --build-arg GIT_COMMIT=$(git rev-parse HEAD) \
  -t illuminator:latest .
```

---

## 使用方法

### 守护进程模式

启动 Illuminator 守护进程，开启 HTTP + SSE 服务（默认端口 9527）：

```bash
sudo ./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example
```

> 注意：eBPF 数据采集需要 root 权限。

服务启动后可访问：

**Web 界面**
- `http://localhost:9527` — 可视化仪表盘（CPU 概览 / 进程 / 火焰图 / 调度器）

**REST API — 传统管道接口**
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
- `POST /api/v1/query` — SQL 查询（只读 SELECT，返回 JSON 行数据）
- `GET /api/v1/internal_metrics` — 内部指标（JSON）

**REST API — 动态 Feature 控制接口（热插拔）**
- `GET /api/v1/features` — 列出所有已注册 Feature 及其状态
- `POST /api/v1/features/:name/start` — 启动指定 Feature（触发管道创建）
- `POST /api/v1/features/:name/stop` — 停止指定 Feature（销毁管道释放资源）
- `GET /api/v1/features/:name/collect` — 获取 Feature 最新数据快照
- `GET /api/v1/features/:name/stream?cursor=N` — 增量拉取（cursor 机制，实时流）
- `POST /api/v1/features/:name/record/start` — 开始录制（数据落盘为 .ilr 文件）
- `POST /api/v1/features/:name/record/stop` — 停止录制
- `GET /api/v1/features/:name/record/status` — 录制状态查询

**SSE (Server-Sent Events)**
- `POST /api/v1/events/subscribe` — 订阅 Feature 数据流
- `GET  /api/v1/events/{id}` — SSE 数据连接
- `POST /api/v1/events/{id}/update` — 动态更新订阅

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
  # SSE 通过同一 HTTP 端口提供

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
        # BPF 字节码已通过 skeleton 嵌入二进制，无需指定 bpf_object 路径
    processors:
      - type: stack_symbolizer
      - type: stack_merger
    aggregator:
      type: cpu_stats_aggregator
      config:
        window_sec: 30
    sinks:
      - type: local_storage
      - type: stream_sink
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
        .api_version = IL_PLUGIN_API_VERSION,
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

## 持续集成 (CI)

项目配置了 GitHub Actions 自动化流水线，位于 `.github/workflows/`。

### CI 流水线 (`ci.yml`)

每次 push 或 PR 自动触发，覆盖后端、前端、eBPF 三条构建链：

| Job | 触发 | 功能 |
|-----|------|------|
| **backend-build** | push/PR | `bazel build //src/...` 全量 C++ 编译 |
| **backend-test** | push/PR | `bazel test //src/...` 运行 25 个单元测试 |
| **bpf-probes** | push/PR | `bazel build //src/ebpf/probes:all` eBPF 探针编译 |
| **frontend-build** | push/PR | `tsc --noEmit` + ESLint + Vitest + `vite build` |
| **sanitizer-asan** | 仅 PR | AddressSanitizer 内存错误检测 |
| **sanitizer-tsan** | 仅 PR | ThreadSanitizer 数据竞争检测 |
| **ci-gate** | 始终 | 汇总门禁，所有必要 job 通过后才允许合并 |
| **docker-build** | 手动 (workflow_dispatch) | Docker 镜像构建验证 |

### 安全扫描 (`security.yml`)

| Job | 频率 | 功能 |
|-----|------|------|
| **codeql-cpp** | 每周 / 手动 | C++ 安全漏洞分析 (SQL 注入、缓冲区溢出) |
| **codeql-js** | 每周 / 手动 | JavaScript/TypeScript 安全分析 |
| **npm-audit** | 每周 + `package*.json` 变更时 | npm 依赖漏洞审计 |

---

## 测试

### 测试架构

项目采用 **GoogleTest** 框架，测试代码遵循就近放置原则：每个组件在其源码目录下创建 `test/` 子目录存放对应的单元测试。

### 测试目录结构

```
src/
├── core/
│   ├── common/test/           # Status, ConfigValue 测试
│   ├── memory/test/           # Arena, LockFreeQueue 测试
│   ├── threading/test/        # ThreadPool 测试
│   └── engine/test/           # TimerWheel, AsyncChannel, DataBatch, Pipeline 集成测试
├── processors/
│   ├── passthrough/test/      # PassthroughProcessor 测试
│   ├── filter/test/           # FilterProcessor 测试
│   ├── stack_merger/test/     # StackMergerProcessor 测试
│   └── stack_symbolizer/test/ # StackSymbolizerProcessor 测试
├── aggregators/
│   └── cpu_stats_aggregator/test/  # CpuStatsAggregator 测试
└── sinks/
    ├── console_output/test/        # ConsoleSink 测试
    ├── fanout/test/                # SinkFanout 多路分发测试
    ├── file_export/test/           # FileExportSink 测试
    ├── local_storage/test/         # LocalStorageSink + SQLite 测试
    ├── otlp_export/test/           # OtlpExportSink 测试
    ├── pprof_export/test/          # PprofExportSink 测试
    ├── prometheus_exposition/test/ # PrometheusSink 测试
    └── stream_sink/test/           # StreamSink + StreamBuffer 测试
```

### 运行测试

```bash
# 运行全部测试目标（后端 C++）
bazel test //src/...

# 运行前端测试
cd web && npm test

# 运行单个模块的测试
bazel test //src/core/engine/test:all

# 运行单个测试目标并输出详细信息
bazel test //src/core/engine/test:pipeline_integration_test --test_output=all
```

### 测试覆盖范围

| 层级 | 组件 | 测试目标数 | 覆盖内容 |
|------|------|-----------|---------|
| **Core Infra** | Status, ConfigValue | 2 | 错误码、StatusOr、类型转换、嵌套配置 |
| **Core Memory** | Arena, LockFreeQueue | 2 | 分配对齐、CopyString、MPSC 并发、水位线 |
| **Core Threading** | ThreadPool | 1 | Submit/Future、异常恢复、析构等待 |
| **Core Engine** | TimerWheel, AsyncChannel, DataBatch | 3 | timerfd 定时、variant dispatch、反压、InternString |
| **Integration** | Pipeline E2E | 1 | Source→Sink 数据流、统计计数器、错误路径 |
| **Processors** | passthrough, filter, stack_merger, stack_symbolizer | 4 | 透传、标签过滤、堆栈合并分组、符号化 |
| **Aggregators** | cpu_stats_aggregator | 1 | 窗口聚合、avg/min/max/p50/p99、Flush 清空 |
| **Sinks** | console, file, local_storage, otlp, pprof, prometheus, recording, fanout | 8 | I/O 写入、格式化、SSE 推送、录制落盘、多路分发 |
| **Sources** | cpu_utilization | 1 | Init/Collect、配置解析、Load Average |
| **Server** | api_routes, auth middleware | 1 | /healthz、认证绕过、401/403/200 |
| **Plugin** | PluginRegistry | 1 | 注册/创建/列举、Source/Processor/Sink |
| **Storage** | SQLite backend | 1 | 并发写入、查询反序列化、只读 SQL |
| **后端总计** | | **25** | |
| **前端** | Zustand stores, hooks | 2 files / 7 cases | useTimeStore、usePolling |

---

## Web 前端功能

### 全局交互

| 功能 | 说明 |
|------|------|
| **TimeControls** | 顶部工具栏，LIVE/PAUSED 模式切换，30s/1m/5m/15m 时间窗口选择 |
| **StatusBar** | 底部状态栏，显示运行管道数和 SSE 连接状态 |
| **键盘快捷键** | `Space` 暂停/恢复、`T` 切换时间窗口、`?` 显示帮助 |

### 页面功能

| 页面 | 路由 | 核心功能 |
|------|------|----------|
| **Overview** | `/` | 系统概览：版本信息、Feature 状态表格、统计卡片 |
| **CPU** | `/cpu` | CPU 利用率趋势图、Per-core 热力图、进程/线程 CPU 排行（System/Process 双 Tab） |
| **Memory** | `/memory` | 内存监控（WIP） |
| **IO** | `/io` | IOPS、IO 延迟监控（依赖 io_monitor feature） |
| **Network** | `/network` | 网络流量、TCP 连接、重传统计（依赖 net_tracer feature） |
| **GPU** | `/gpu` | GPU 监控（WIP） |
| **Replay** | `/replay` | 录制回放 |
| **Query** | `/query` | SQL 查询控制台（只读 SELECT）、示例查询、执行耗时统计 |
| **Plugins** | `/plugins` | Feature Health Dashboard、热插拔管理 |
| **System** | `/system` | 系统健康状态、Pipeline 详情表格（RUN/STOP/batch/record 计数）、内部指标 JSON |

---

## 版本机制

版本号在编译时通过 Bazel stamp 机制自动注入：

| 场景 | 版本号格式 | 示例 |
|------|-----------|------|
| 有精确 git tag（`v1.2.3`） | `1.2.3` | Release 版本 |
| 有历史 tag | `1.2.3-5-gabcdef0` | 开发中版本 |
| 无 tag | `abcdef0` | 8 位 commit hash |

**后端**：通过 `tools/workspace_status.sh` → Bazel stamp → `version_generated.h`，启动时输出版本并在 `/healthz` 返回。

**前端**：Vite 构建时注入 `__APP_VERSION__`，运行时从 `/healthz` 动态获取。标题旁显示。

**Docker**：通过 `--build-arg GIT_VERSION=xxx GIT_COMMIT=yyy` 传入。

---

## 文档索引

| 文档 | 说明 |
|------|------|
| [dynamic_plugin_architecture.md](docs/dynamic_plugin_architecture.md) | 动态插件架构设计 v2.0（热插拔 + 录制回放） |
| [cpu_monitoring_design.md](docs/cpu_monitoring_design.md) | CPU 监控功能设计（USE 方法论 + PMC + PSI） |
| [pipeline_v3_design.md](docs/pipeline_v3_design.md) | Pipeline v3 事件驱动架构设计 |
| [perf_ebpf_comparison.md](docs/perf_ebpf_comparison.md) | 与 perf_ebpf 的架构对比分析 |
| [architecture_audit_v4.md](docs/architecture_audit_v4.md) | 全面架构审计报告 (P0-P2 缺陷追踪) |
| [wasm_runtime_design.md](docs/wasm_runtime_design.md) | WASM 沙箱插件系统设计与路线图 |
| [onboarding_guide.md](docs/onboarding_guide.md) | 新人入门指南 |

---

## 许可证

MIT License
