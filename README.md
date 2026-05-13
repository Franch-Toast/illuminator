# Illuminator

**高性能、插件化的全栈可观测性与深度性能分析平台**

基于 C++ 实现，利用 eBPF 技术在 Linux 系统上进行零侵入数据采集，覆盖 CPU、内存、网络、磁盘 I/O、调度器等多个领域，配套自研 Web 可视化平台并兼容业界标准格式导出。

---

## 核心特性

- **eBPF 零侵入采集**：基于 libbpf + CO-RE 的现代 eBPF 模式，支持 CPU 性能剖析、内存分配追踪、网络连接监控、块 I/O 延迟分析、调度事件追踪
- **管道式插件架构**：`Source → Processor → Aggregator → Sink` 四阶段管道，支持 Pull/Push 双模式、背压控制
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
  │                     Core Engine (C++17)                          │
  │                                                                  │
  │  Source ──▶ Processor ──▶ Aggregator ──▶ Sink                   │
  │                                                                  │
  │  Pipeline Controller (Pull/Push 双模式 / 背压 / 流量整形)      │
  │  Shared Infra: Arena + Lock-Free Queue + Thread Pool             │
  │  eBPF Subsystem: libbpf Loader + Ring Buffer + BTF Cache         │
  │  Plugin Manager: SO Loader + WASM Runtime + Plugin Registry      │
  │  Storage Layer: SQLite (WAL 模式)                                │
  │  Export Layer: pprof / OTLP / Prometheus / JSON                  │
  │  Self-Observability: 内部指标 / 健康检查 / 资源限制器            │
  └──────────────────────┬─────────────────────────────────────────┘
                         │ HTTP (cpp-httplib) / WebSocket (独立端口)
  ┌──────────────────────▼─────────────────────────────────────────┐
  │              Web 可视化平台 (React + TypeScript)                 │
  │  Dashboard │ Flame Graph │ Timeline │ Diff View │ Query Console │
  └────────────────────────────────────────────────────────────────┘
```

---

## 项目结构

```
illuminator/
├── MODULE.bazel                # Bazel bzlmod 依赖管理
├── .bazelrc                    # Bazel 编译配置（C++17, sanitizers 等）
├── illuminator.yaml.example    # 示例配置文件
├── README.md                   # 本文件
│
├── src/                        # ===== 全部 C++ 源代码 =====
│   ├── core/                   # 核心引擎
│   │   ├── common/             #   Status, Logger(spdlog), Config, StringUtil, 自观测
│   │   ├── config/             #   YAML 配置加载器
│   │   ├── engine/             #   PipelineController, DataBatch
│   │   ├── memory/             #   Arena(零拷贝), Lock-Free Queue(MPSC/MPMC)
│   │   └── threading/          #   ThreadPool, ThreadUtil(线程命名)
│   │
│   ├── plugin/                 # 插件框架
│   │   ├── api/                #   插件接口 (Source/Processor/Aggregator/Sink)
│   │   ├── manager/            #   Registry(线程安全), SO Loader, WASM Runtime
│   │   └── builtin/            #   内建插件注册
│   │
│   ├── ebpf/                   # eBPF 子系统
│   │   ├── include/            #   vmlinux.h, event_types.h, common.bpf.h
│   │   ├── probes/             #   BPF 探针源码 (按子系统分类)
│   │   │   ├── cpu/            #     cpu_profiler.bpf.c, cpu_sampler.bpf.c
│   │   │   ├── sched/          #     sched_analyzer.bpf.c, sched_tracer.bpf.c, offcpu_profiler.bpf.c
│   │   │   ├── io/             #     bio_latency.bpf.c
│   │   │   ├── net/            #     net_tracer.bpf.c
│   │   │   └── memory/         #     mem_tracer.bpf.c
│   │   └── loader/             #   BpfProgramManager, FeatureProbe, StackTraceUtil
│   │
│   ├── sources/                # Source 插件 (按子系统分类)
│   │   ├── cpu/                #   9 个 CPU 相关源
│   │   │   ├── cpu_profiler/   #     eBPF perf_event CPU 性能剖析
│   │   │   ├── cpu_utilization/#     统一 CPU 利用率 (EMA 平滑)
│   │   │   ├── ebpf_cpu_sampler/#    eBPF CPU 采样
│   │   │   ├── process_cpu/    #     进程 CPU 监控 (Top-N, 线程详情)
│   │   │   ├── cpu_sys_monitor/#     系统级 CPU 指标
│   │   │   ├── cpu_sys_stats/  #     CPU 统计
│   │   │   ├── proc_stat_reader/#    /proc/stat 读取
│   │   │   ├── proc_cpu_monitor/#    进程 CPU 监控 (遗留)
│   │   │   └── process_cpu_monitor/# 按进程/线程 CPU 利用率
│   │   ├── sched/              #   3 个调度器相关源
│   │   │   ├── sched_analyzer/ #     调度分析 (运行队列延迟, 迁移追踪)
│   │   │   ├── ebpf_sched_tracer/#   eBPF 调度事件追踪
│   │   │   └── offcpu_profiler/#     Off-CPU 性能剖析
│   │   ├── io/                 #   1 个 I/O 相关源
│   │   │   └── ebpf_io_monitor/#     eBPF 块 I/O 延迟监控
│   │   └── net/                #   1 个网络相关源
│   │       └── ebpf_net_tracer/#     eBPF TCP 连接追踪
│   │
│   ├── processors/             # Processor 插件
│   │   ├── passthrough/        #   透传处理器 (测试用)
│   │   ├── filter/             #   标签过滤处理器
│   │   ├── stack_symbolizer/   #   堆栈符号化 (ELF + kallsyms + C++ demangle)
│   │   └── stack_merger/       #   相同调用栈合并
│   │
│   ├── aggregators/            # Aggregator 插件
│   │   └── cpu_stats_aggregator/# CPU 统计聚合
│   │
│   ├── sinks/                  # Sink 插件
│   │   ├── console_output/     #   控制台输出
│   │   ├── file_export/        #   JSONL 文件导出
│   │   ├── local_storage/      #   SQLite 存储后端写入
│   │   ├── pprof_export/       #   pprof 折叠栈格式导出
│   │   ├── prometheus_exposition/#  Prometheus 指标暴露
│   │   ├── otlp_export/        #   OpenTelemetry OTLP 导出
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
│   │   ├── http_server.h       #   cpp-httplib 薄封装 (生命周期管理)
│   │   ├── websocket_server.h  #   WebSocket 协议实现 (内联 SHA-1)
│   │   └── websocket_manager.h #   WebSocket 连接管理 + 独立监听端口
│   │
│   └── cli/                    # 命令行入口
│       └── main.cc             #   daemon / collect / top / plugins / version
│
├── web/                        # ===== Web 前端 (React + TypeScript) =====
│   ├── package.json            #   npm 依赖
│   ├── vite.config.ts          #   Vite 构建配置
│   ├── src/
│   │   ├── App.tsx             #   路由: / /flamegraph /timeline /diff /query
│   │   ├── hooks/
│   │   │   ├── useApi.ts       #     API hooks
│   │   │   └── useWebSocket.ts #     WebSocket 自动重连 hook
│   │   └── pages/              #   页面组件
│   │       ├── Dashboard.tsx   #     CPU 概览仪表盘 (实时 + EMA)
│   │       ├── FlameGraph.tsx  #     火焰图 (On-CPU / Off-CPU)
│   │       └── Timeline.tsx    #     调度器分析 (时序/迁移/Wakeup)
│   └── dist/                   #   前端构建产物
│
├── third_party/                # ===== 第三方库 (vendored) =====
│   └── cpp-httplib/            #   cpp-httplib v0.16.3 (单头文件)
│
└── tools/                      # ===== 开发工具 =====
    └── bpf/
        └── compile_probes.sh   #   BPF 探针编译脚本 (已有 Bazel 替代)
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

### Vendored 依赖

| 库 | 版本 | 用途 |
|----|------|------|
| cpp-httplib | 0.16.3 | HTTP 服务器 (单头文件) |

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

也可使用脚本（遗留方式）：

```bash
tools/bpf/compile_probes.sh
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
- `http://localhost:9527` — Web 可视化界面
- `http://localhost:9527/healthz` — 健康检查
- `http://localhost:9527/metrics` — Prometheus 指标
- `http://localhost:9527/api/v1/pipelines` — 管道状态
- `http://localhost:9527/api/v1/cpu/utilization` — CPU 利用率
- `http://localhost:9527/api/v1/cpu/processes` — 进程 CPU 指标
- `http://localhost:9527/api/v1/cpu/profile/flamegraph` — On-CPU 火焰图
- `http://localhost:9527/api/v1/cpu/profile/offcpu` — Off-CPU 火焰图
- `http://localhost:9527/api/v1/cpu/sched/summary` — 调度器摘要
- `http://localhost:9527/api/v1/cpu/sched/history` — 调度历史
- `http://localhost:9527/api/v1/cpu/sched/events` — 调度事件
- `http://localhost:9527/api/v1/cpu/sched/wakeups` — Wakeup 链
- `http://localhost:9527/api/v1/internal_metrics` — 内部指标（JSON）
- `ws://localhost:9528/ws/<pipeline>` — WebSocket 实时数据推送

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
    listen: "0.0.0.0:9528"

pipelines:
  cpu_profiling:
    source:
      type: cpu_profiler
      config:
        frequency_hz: 49
        bpf_object: build/bpf/cpu_profiler.bpf.o
    processors:
      - type: stack_symbolizer
      - type: stack_merger
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
