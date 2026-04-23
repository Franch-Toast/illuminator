# Illuminator

**高性能、插件化的全栈可观测性与深度性能分析平台**

基于 C++ 实现，利用 eBPF 技术在 Linux 系统上进行零侵入数据采集，覆盖 CPU、内存、网络、磁盘 I/O、调度器等多个领域，配套自研 Web 可视化平台并兼容业界标准格式导出。

---

## 核心特性

- **eBPF 零侵入采集**：基于 libbpf + CO-RE 的现代 eBPF 模式，支持 CPU 性能剖析、内存分配追踪、网络连接监控、块 I/O 延迟分析、调度事件追踪
- **管道式插件架构**：`Source → Processor → Aggregator → Sink` 四阶段管道，支持 DAG 编排、背压控制、热加载
- **三层插件系统**：
  - **Builtin**（内建）：编译时链接，零开销
  - **Shared Object**（动态库）：运行时 `.so` 加载，稳定 C ABI
  - **WASM**（沙箱）：多语言编写，内存隔离
- **存储抽象层**：可插拔后端（SQLite 默认、DuckDB、ClickHouse、Parquet）
- **多格式导出**：pprof、OTLP、Prometheus、Perfetto、Folded Stacks、JSON
- **Web 可视化平台**：实时仪表盘、火焰图、时间线视图、对比分析、查询控制台
- **自观测能力**：内部指标、健康检查、资源限制器

---

## 架构概览

```
                    ┌───────────────────────────────────┐
                    │       Illuminator 总体架构         │
                    └───────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────────┐
  │                    Configuration Layer                          │
  │  YAML/JSON Pipeline Config  │  Plugin Manifest  │  CLI / API   │
  └──────────────────────┬─────────────────────────────────────────┘
                         │
  ┌──────────────────────▼─────────────────────────────────────────┐
  │                     Core Engine (C++)                            │
  │                                                                  │
  │  Source ──▶ Processor ──▶ Aggregator ──▶ Sink                   │
  │                                                                  │
  │  Pipeline Controller (DAG 调度 / 背压 / 流量整形 / 优先级)      │
  │  Shared Infra: Arena + Lock-Free Queue + Thread Pool             │
  │  eBPF Subsystem: libbpf Loader + Ring Buffer + BTF Cache         │
  │  Plugin Manager: SO Loader + WASM Runtime + Plugin Registry      │
  │  Storage Layer: SQLite / DuckDB / ClickHouse / Parquet           │
  │  Export Layer: pprof / OTLP / Prometheus / Perfetto / JSON       │
  │  Self-Observability: 内部指标 / 健康检查 / 资源限制器            │
  └──────────────────────┬─────────────────────────────────────────┘
                         │ HTTP / WebSocket
  ┌──────────────────────▼─────────────────────────────────────────┐
  │              Web 可视化平台 (React + TypeScript)                 │
  │  Dashboard │ Flame Graph │ Timeline │ Diff View │ Query Console │
  │  模式: Online(实时) │ Offline(文件导入) │ Report(自包含HTML)    │
  └────────────────────────────────────────────────────────────────┘
```

---

## 项目结构

```
illuminator/
├── BUILD                       # 根 Bazel 构建文件
├── MODULE.bazel                # Bazel bzlmod 依赖管理
├── MODULE.bazel.lock           # 依赖锁文件
├── .bazelrc                    # Bazel 编译配置（C++20, sanitizers 等）
├── illuminator.yaml.example    # 示例配置文件
├── README.md                   # 本文件
│
├── src/                        # ===== 全部 C++ 源代码 =====
│   ├── core/                   # 核心引擎
│   │   ├── common/             #   通用基础: Status, Logger, Config, 自观测
│   │   ├── engine/             #   管道引擎: PipelineController, DataBatch
│   │   ├── memory/             #   内存管理: Arena(零拷贝), Lock-Free Queue
│   │   └── threading/          #   线程池
│   │
│   ├── plugin/                 # 插件框架
│   │   ├── api/                #   插件接口定义 (Source/Processor/Aggregator/Sink)
│   │   ├── manager/            #   插件管理: Registry, SO Loader, WASM Runtime
│   │   └── builtin/            #   内建插件注册
│   │
│   ├── ebpf/                   # eBPF 子系统
│   │   ├── include/            #   共享头文件: vmlinux.h, event_types.h
│   │   ├── probes/             #   BPF 程序源码 (.bpf.c)
│   │   └── loader/             #   BPF 程序管理器, 内核特性探测
│   │
│   ├── sources/                # Source 插件实现
│   │   ├── proc_stat_reader/   #   /proc 文件系统读取 (CPU/内存/负载)
│   │   ├── ebpf_cpu_sampler/   #   eBPF CPU 性能采样
│   │   ├── ebpf_net_tracer/    #   eBPF 网络连接追踪
│   │   ├── ebpf_io_monitor/    #   eBPF 块 I/O 延迟监控
│   │   └── ebpf_sched_tracer/  #   eBPF 调度器事件追踪
│   │
│   ├── processors/             # Processor 插件实现
│   │   ├── passthrough/        #   透传处理器 (测试用)
│   │   └── filter/             #   标签过滤处理器
│   │
│   ├── sinks/                  # Sink 插件实现
│   │   ├── console_output/     #   控制台输出
│   │   ├── file_export/        #   JSONL 文件导出
│   │   ├── local_storage/      #   本地存储后端写入
│   │   ├── pprof_export/       #   pprof 折叠栈格式导出
│   │   ├── prometheus_exposition/ # Prometheus 指标暴露
│   │   └── otlp_export/        #   OpenTelemetry OTLP 导出
│   │
│   ├── storage/                # 存储抽象层
│   │   ├── storage_backend.h   #   StorageBackend 接口 + StorageFactory
│   │   └── sqlite_backend/     #   SQLite 存储实现 (WAL 模式)
│   │
│   ├── server/                 # HTTP/WebSocket 服务
│   │   └── http_server.h       #   嵌入式 HTTP 服务器
│   │
│   └── cli/                    # 命令行工具入口
│       └── main.cc             #   daemon / collect / top / export / plugins 命令
│
├── web/                        # ===== Web 前端 (React + TypeScript) =====
│   ├── package.json            #   npm 依赖
│   ├── vite.config.ts          #   Vite 构建配置
│   ├── index.html              #   入口 HTML
│   ├── src/
│   │   ├── App.tsx             #   应用主组件 + 路由
│   │   ├── main.tsx            #   React 入口
│   │   ├── hooks/useApi.ts     #   API hooks (管道、健康检查、指标)
│   │   └── pages/              #   页面组件
│   │       ├── Dashboard.tsx   #     实时仪表盘
│   │       ├── FlameGraph.tsx  #     火焰图
│   │       ├── Timeline.tsx    #     时间线
│   │       ├── DiffView.tsx    #     对比视图
│   │       └── QueryConsole.tsx#     查询控制台
│   └── dist/                   #   前端构建产物
│
├── tools/                      # ===== 构建 & 开发工具 =====
│   ├── bpf/
│   │   └── compile_probes.sh   #   BPF 探针编译脚本
│   └── bazel/                  #   自定义 Bazel 规则 (预留)
│
├── tests/                      # ===== 测试 =====
│   ├── unit/                   #   单元测试
│   ├── integration/            #   集成测试
│   └── benchmark/              #   性能基准测试
│
└── docs/                       # ===== 文档 =====
```

---

## 依赖

### 系统依赖

| 依赖 | 最低版本 | 用途 |
|------|---------|------|
| **Linux 内核** | 5.8+ | eBPF Ring Buffer 支持 (4.14+ 可降级使用) |
| **Bazel** | 7.0+ | 构建系统 (推荐使用 Bazelisk) |
| **Clang** | 14+ | BPF 探针编译 (`-target bpf`) |
| **libbpf** | 1.0+ | eBPF 程序加载器 |
| **libelf + zlib** | - | ELF 解析 (libbpf 依赖) |
| **SQLite3** | 3.35+ | 默认存储后端 |
| **Node.js** | 18+ | Web 前端构建 (可选) |

### Bazel 管理的 C++ 依赖 (自动下载)

| 库 | 版本 | 用途 |
|----|------|------|
| abseil-cpp | 20240722.0 | 基础工具库 |
| nlohmann/json | 3.11.3 | JSON 解析 |
| yaml-cpp | 0.8.0 | YAML 配置解析 |
| spdlog | 1.14.1 | 结构化日志 |
| fmt | 10.2.1 | 格式化库 |
| googletest | 1.15.2 | 单元测试框架 |

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

# (可选) BPF 工具
sudo apt install -y bpftool linux-tools-common
```

### 构建 C++ 后端

```bash
# 标准构建
bazel build //src/cli:illuminator

# 优化构建
bazel build //src/cli:illuminator --config=opt

# Debug 构建
bazel build //src/cli:illuminator --config=dbg

# 带 AddressSanitizer 构建
bazel build //src/cli:illuminator --config=asan
```

### 编译 BPF 探针 (可选)

```bash
tools/bpf/compile_probes.sh
```

编译后的 `.bpf.o` 文件输出到 `build/bpf/` 目录。

### 构建 Web 前端 (可选)

```bash
cd web
npm install
npm run build
```

构建产物输出到 `web/dist/`，会被 C++ HTTP 服务器自动托管。

---

## 使用方法

### 守护进程模式

启动 Illuminator 守护进程，开启 HTTP 服务 (默认端口 9527) 和实时数据采集管道：

```bash
./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example
```

服务启动后可访问：
- `http://localhost:9527` — Web 可视化界面
- `http://localhost:9527/healthz` — 健康检查
- `http://localhost:9527/metrics` — Prometheus 指标
- `http://localhost:9527/api/v1/pipelines` — 管道状态

### 一次性采集

```bash
# 采集 30 秒系统指标
./bazel-bin/src/cli/illuminator collect --duration 30

# 指定日志级别
./bazel-bin/src/cli/illuminator collect --duration 10 --log-level debug
```

### 实时系统概览 (类 top)

```bash
./bazel-bin/src/cli/illuminator top
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

pipelines:
  cpu_profiling:
    source:
      type: ebpf_cpu_sampler
      config:
        frequency_hz: 49
    processors:
      - type: filter
    sinks:
      - type: console_output
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
