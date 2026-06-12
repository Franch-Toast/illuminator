# Illuminator 项目入门指南与未来展望

> **文档目的**：为首次接触本项目的开发者提供系统性的学习路径，帮助快速理解项目架构、核心概念和代码组织。同时为团队提供未来开发的方向参考。
>
> **适用人群**：C++ 后端开发者、系统工程师、对 eBPF/可观测性感兴趣的开发者
>
> **预计阅读时间**：30-40 分钟精读 + 2-3 天实操探索

---

## 目录

1. [项目是什么](#1-项目是什么)
2. [10 分钟快速上手](#2-10-分钟快速上手)
3. [学习路线图：从基础到精通](#3-学习路线图从基础到精通)
4. [核心概念详解](#4-核心概念详解)
5. [代码阅读顺序（文件级指引）](#5-代码阅读顺序文件级指引)
6. [如何新增一个插件（端到端示例）](#6-如何新增一个插件端到端示例)
7. [测试体系导航](#7-测试体系导航)
8. [常见问题与陷阱](#8-常见问题与陷阱)
9. [未来展望与开发路线图](#9-未来展望与开发路线图)

---

## 1. 项目是什么

Illuminator 是一个 **高性能、插件化的全栈可观测性平台**。它通过 eBPF 技术在 Linux 上进行零侵入数据采集（CPU、调度器、I/O、网络），配套自研 Web 可视化平台，支持多种标准格式导出。

**一句话总结**：`eBPF 数据采集 → 异步管道处理 → 多格式存储/可视化`

### 技术栈概览

| 层级 | 技术选择 | 理由 |
|------|---------|------|
| 语言 | C++17 | 性能敏感、系统级编程 |
| 构建 | Bazel (bzlmod) | 可重复构建、依赖管理 |
| 数据采集 | eBPF (libbpf + CO-RE) | 零侵入、内核级可观测 |
| 日志 | spdlog | 高性能、fmt 风格 |
| 配置 | yaml-cpp | 人类友好、结构化 |
| 序列化 | nlohmann/json | Header-only、易用 |
| HTTP | cpp-httplib | 单头文件、轻量 |
| 存储 | SQLite (WAL) | 嵌入式、零运维 |
| 前端 | React + TypeScript + Vite | 现代 SPA |
| 图表 | Recharts + d3-flame-graph | 火焰图专业渲染 |

---

## 2. 10 分钟快速上手

### 2.1 环境准备

```bash
# 安装系统依赖 (Ubuntu 22.04+)
sudo apt install -y clang llvm libbpf-dev libelf-dev zlib1g-dev libsqlite3-dev

# 安装 Bazel (推荐 Bazelisk)
sudo npm install -g @bazel/bazelisk
```

### 2.2 构建与运行

```bash
cd illuminator

# 构建后端
bazel build //src/cli:illuminator

# 运行测试（验证环境可用）
bazel test //src/...

# 启动守护进程（需要 root，因为 eBPF）
sudo ./bazel-bin/src/cli/illuminator daemon --config illuminator.yaml.example
```

### 2.3 访问界面

- Web 界面: `http://localhost:9527`
- 健康检查: `curl http://localhost:9527/healthz`
- 管道状态: `curl http://localhost:9527/api/v1/pipelines`
- Prometheus: `curl http://localhost:9527/metrics`

---

## 3. 学习路线图：从基础到精通

### 阶段一：理解数据流（Day 1）

**目标**：理解"数据从哪来、到哪去"

```
Source (数据源) → AsyncChannel → Processor (处理器) → Aggregator (聚合器) → Sink (输出端)
```

**必读文件**：
1. `src/core/engine/data_batch.h` — 理解流水线中流动的数据长什么样
2. `src/plugin/api/source_plugin.h` — Pull 模式 vs Push 模式
3. `src/plugin/api/sink_plugin.h` — 数据最终去向

**实验**：
```bash
# 启动后，调用一次 CPU 采集，观察返回的 JSON 结构
curl http://localhost:9527/api/v1/cpu/utilization | python3 -m json.tool
```

### 阶段二：理解线程模型（Day 1-2）

**目标**：理解 v3 事件驱动架构的线程分工

```
TimerWheel (1 线程)       — 纯调度，不执行工作
    ├→ CollectPool (M 线程)  — 并行执行 Source::Collect()
    └→ InjectFlush          — 注入 FlushSentinel 到 channel

AsyncChannel (无锁队列)    — MPSC，variant<DataBatch, FlushSentinel>
    └→ ProcessThread (每管道 1)  — 纯事件处理器
         ├ DataBatch → processors → aggregator.Add → sinks
         └ FlushSentinel → aggregator.Flush → sinks

SinkPool (K 线程)          — 并行执行 Sink::Write()
```

**必读文件**：
1. `docs/pipeline_v3_design.md` — 架构设计文档，理解"为什么这样设计"
2. `src/core/engine/pipeline_controller.h` — `Pipeline` 类的 `Start()` 和 `ProcessLoop()`
3. `src/core/engine/pipeline_controller.cc` — `BuildFromConfig()` 和 `StartAll()` 看编排逻辑

### 阶段三：理解配置系统（Day 2）

**目标**：理解 YAML 如何驱动整个系统的启动

```
illuminator.yaml.example
    └→ YamlConfigLoader::LoadFromFile()
        └→ GlobalConfig { engine, pipelines[], server }
            └→ PipelineController::BuildFromConfig(config)
                └→ PluginRegistry::CreateSource/Processor/Aggregator/Sink
```

**必读文件**：
1. `illuminator.yaml.example` — 完整配置参考
2. `src/core/common/config.h` — `ConfigValue`, `PipelineConfig`, `GlobalConfig` 三层结构
3. `src/core/config/yaml_config_loader.h` — YAML → GlobalConfig 的转换逻辑

### 阶段四：理解插件系统（Day 2-3）

**目标**：掌握如何添加新功能

**必读文件**：
1. `src/plugin/manager/plugin_registry.h` — 宏注册机制 `IL_REGISTER_*`
2. `src/sources/cpu/cpu_utilization/cpu_utilization.h` — 最简单的 Pull Source 参考
3. `src/processors/filter/filter_processor.h` — 标签过滤 Processor 参考
4. `src/sinks/console_output/console_sink.h` — 最简单的 Sink 参考

### 阶段五：深入基础设施（Day 3+）

**目标**：理解性能优化和工程决策

**必读文件**：
1. `src/core/memory/arena.h` — 零拷贝 bump-pointer 内存分配
2. `src/core/memory/lock_free_queue.h` — MPSC 无锁环形缓冲区
3. `src/core/engine/async_channel.h` — 反压水位线、丢弃策略、三级退避
4. `src/core/engine/timer_wheel.h` — timerfd + epoll 实现
5. `src/core/threading/thread_pool.h` — 通用线程池

### 阶段六：eBPF 子系统（专项）

**前置知识**：BPF 基础、libbpf 用法

**必读文件**：
1. `src/ebpf/include/event_types.h` — 内核/用户态共享数据结构
2. `src/ebpf/loader/bpf_program_manager.h` — libbpf 封装层
3. `src/sources/ebpf_ring_buffer_source.h` — eBPF Push Source 公共基类
4. `src/ebpf/probes/cpu/cpu_profiler.bpf.c` — BPF C 程序示例

---

## 4. 核心概念详解

### 4.1 DataBatch — 数据的"容器"

`DataBatch` 是管道中流动的基本单位，类似于数据库中的"行集合"。

```cpp
DataBatch
├── Type: kMetrics | kProfile | kTrace | kLog | kGeneric
├── Arena (shared_ptr) — 零拷贝内存池
├── Records[] — 指标数据 (CPU%, 内存, 负载...)
│   ├── Labels: {cpu: "0", host: "node1"}
│   └── Fields: {usage_percent: 45.3, idle: 54.7}
└── StackSamples[] — 性能剖析数据 (火焰图)
    ├── comm: "nginx"
    ├── Frames: [main, handle_request, read_socket, ...]
    └── count: 42
```

**关键设计**：Arena 内存池支持零拷贝。所有字符串通过 `InternString()` 存入 Arena，生命周期由 `shared_ptr<DataBatch>` 管理。

### 4.2 AsyncChannel — 异步通信"管道"

AsyncChannel 连接生产者（Source）和消费者（ProcessThread），传输两种消息：

| 消息类型 | 用途 | 来源 |
|---------|------|------|
| `DataBatchPtr` | 正常数据批次 | Source::Collect() 或 Push callback |
| `FlushSentinel` | 触发 Aggregator flush | TimerWheel 周期注入 |

**反压机制**：队列使用率超过 `backpressure_high`（默认 80%）时触发反压，低于 `backpressure_low`（默认 20%）时解除。

**丢弃策略**：队列满时有两种选择：
- `drop_newest`：丢弃新数据（保守，保留历史）
- `drop_oldest`：丢弃旧数据（激进，保留最新）

### 4.3 TimerWheel — 全局"心跳"

TimerWheel 是整个系统的节拍器，使用 Linux timerfd + epoll 实现：

- **不执行任何实际工作**——只做 dispatch
- 回调函数要求纳秒级完成（提交到线程池或注入 Sentinel）
- 管理三类周期事件：
  1. Pull Source 的 `Collect()` 调度 → CollectPool
  2. Aggregator 的 `Flush` 触发 → InjectFlush 到 AsyncChannel
  3. InternalMetrics 的周期同步

### 4.4 插件系统 — 四种角色

```
Source → 生产数据（Pull/Push 两种模式）
  │
  └→ Processor → 变换/过滤数据（链式，可选多个）
       │
       └→ Aggregator → 时间窗口聚合（可选，最多一个）
            │
            └→ Sink → 输出数据（可多个，并行执行）
```

所有插件通过宏自注册，无需修改中心注册表：
```cpp
IL_REGISTER_SOURCE("my_source", MySource);
IL_REGISTER_PROCESSOR("my_processor", MyProcessor);
IL_REGISTER_AGGREGATOR("my_aggregator", MyAggregator);
IL_REGISTER_SINK("my_sink", MySink);
```

### 4.5 Status/StatusOr — 错误处理范式

项目禁止使用 C++ 异常，所有错误通过返回值传递：

```cpp
// 成功路径
StatusOr<DataBatchPtr> result = source->Collect();
if (result.ok()) {
    auto batch = std::move(*result);
    // 使用 batch
}

// 错误链
auto status = Status::Wrap(inner_status, "Loading config failed");
// ToString() 输出: "Loading config failed <- inner error message"
```

---

## 5. 代码阅读顺序（文件级指引）

以下是推荐的**精确阅读顺序**，每个文件标注了阅读重点：

### 第一轮：建立心智模型（2-3 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 1 | `README.md` | ~527 | 整体架构图、线程模型表、项目结构树 |
| 2 | `docs/pipeline_v3_design.md` | ~796 | v3 设计动机和架构决策（重点看前 200 行） |
| 3 | `illuminator.yaml.example` | ~215 | 配置项全貌，理解系统能力边界 |
| 4 | `src/cli/main.cc` | ~387 | 程序入口，理解 daemon/collect/top 的启动流程 |

### 第二轮：核心数据结构（2 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 5 | `src/core/common/status.h` | ~154 | `StatusCode` 枚举、`Status::Wrap()` 错误链、`StatusOr<T>` 模板 |
| 6 | `src/core/common/config.h` | ~201 | `ConfigValue` 的扁平化设计、`PipelineConfig::StageConfig` |
| 7 | `src/core/engine/data_batch.h` | ~229 | `Record`/`StackSample`/`DataBatch` 三层结构、Arena 零拷贝 |

### 第三轮：管道引擎（3 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 8 | `src/core/memory/arena.h` | ~184 | `AllocateBlock` 分块策略、`SetOomHandler` |
| 9 | `src/core/memory/lock_free_queue.h` | ~159 | CAS 入队/出队算法、序列号协调机制 |
| 10 | `src/core/engine/async_channel.h` | ~181 | `ChannelItem` variant、三级退避 Dequeue、`InjectFlush` 优先级 |
| 11 | `src/core/engine/timer_wheel.h` | ~232 | timerfd/epoll/eventfd 三件套、优先队列 arm 策略 |
| 12 | `src/core/threading/thread_pool.h` | ~142 | mutex+CV 模型、Submit 返回 future |
| 13 | `src/core/engine/pipeline_controller.h` | ~418 | `Pipeline` 生命周期（Start→ProcessLoop→Stop）、指标暴露 |
| 14 | `src/core/engine/pipeline_controller.cc` | ~198 | `BuildFromConfig` 插件实例化、`StartAll` TimerWheel 注册 |

### 第四轮：插件框架与示例（2 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 15 | `src/plugin/api/source_plugin.h` | ~68 | Pull/Push 双模式、`QueryExtra` 扩展查询 |
| 16 | `src/plugin/api/processor_plugin.h` | ~41 | `Process(DataBatchPtr)` 返回修改后的 batch |
| 17 | `src/plugin/api/aggregator_plugin.h` | ~49 | `Add`/`Flush` 时间窗口模式 |
| 18 | `src/plugin/api/sink_plugin.h` | ~41 | `Write`/`Flush` 输出接口 |
| 19 | `src/plugin/manager/plugin_registry.h` | ~236 | 工厂模式 + `IL_REGISTER_*` 宏展开 |
| 20 | `src/sources/cpu/cpu_utilization/cpu_utilization.h` | ~231 | **最佳入门参考**——典型 Pull Source |
| 21 | `src/processors/filter/filter_processor.h` | ~93 | 标签过滤逻辑，简洁的 Processor 示例 |
| 22 | `src/sinks/console_output/console_sink.h` | ~172 | 最简单的 Sink，text/json 双格式 |

### 第五轮：服务层与前端（1.5 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 23 | `src/server/api_routes.h` | ~182 | REST API 全景，`pipeline_collect` 统一处理 |
| 24 | `src/server/websocket_manager.h` | ~315 | WS 订阅模型、BroadcastData 推送 |
| 25 | `web/src/App.tsx` | ~139 | 前端路由、ErrorBoundary、暗色主题 |
| 26 | `web/src/hooks/useApi.ts` | ~108 | REST 数据获取 hooks |

### 第六轮：eBPF 子系统（专项，2 小时）

| 序号 | 文件 | 行数 | 阅读重点 |
|------|------|------|---------|
| 27 | `src/ebpf/include/event_types.h` | ~224 | 内核/用户态共享结构定义 |
| 28 | `src/ebpf/loader/bpf_program_manager.h` | ~289 | libbpf 操作封装 |
| 29 | `src/sources/ebpf_ring_buffer_source.h` | ~122 | eBPF Push Source 公共基类 |
| 30 | `src/ebpf/probes/cpu/cpu_profiler.bpf.c` | - | BPF C 程序实际示例 |

---

## 6. 如何新增一个插件（端到端示例）

以添加一个 **MemoryUsageSource**（内存使用监控）为例：

### Step 1: 创建源文件

```
src/sources/memory/memory_usage/memory_usage.h
```

```cpp
#pragma once
#include "plugin/api/source_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

class MemoryUsageSource : public SourcePlugin {
public:
    const char* Name() const override { return "memory_usage"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        interval_ms_ = config["interval_ms"].AsInt(2000);
        return Status::Ok();
    }

    bool IsPushMode() const override { return false; }  // Pull 模式
    int IntervalMs() const override { return interval_ms_; }

    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>();
        // 读取 /proc/meminfo 并填充 Record...
        return batch;
    }

private:
    int interval_ms_ = 2000;
};

IL_REGISTER_SOURCE("memory_usage", MemoryUsageSource);

}  // namespace illuminator
```

### Step 2: 添加 BUILD 规则

在 `src/sources/BUILD` 中添加：

```python
cc_library(
    name = "memory_usage",
    hdrs = ["memory/memory_usage/memory_usage.h"],
    strip_include_prefix = "",
    include_prefix = "sources",
    visibility = ["//visibility:public"],
    alwayslink = True,  # 确保 IL_REGISTER_* 静态初始化器被链接
    deps = [
        "//src/core:common",
        "//src/plugin:api",
        "//src/plugin:manager",
    ],
)
```

### Step 3: 注册到 builtin 列表

在 `src/plugin/builtin/builtin_plugins.cc` 中添加：

```cpp
#include "sources/memory/memory_usage/memory_usage.h"
```

在 `src/plugin/BUILD` 的 `builtin` target deps 中添加：

```python
"//src/sources:memory_usage",
```

### Step 4: 添加配置

在 `illuminator.yaml.example` 中添加管道定义：

```yaml
pipelines:
  memory_monitor:
    source:
      type: memory_usage
      config:
        interval_ms: 2000
    sinks:
      - type: console_output
        config:
          format: json
```

### Step 5: 编写测试

创建 `src/sources/memory/memory_usage/test/memory_usage_test.cc` 和对应的 `BUILD`。

### Step 6: 验证

```bash
bazel build //src/cli:illuminator
bazel test //src/...
./bazel-bin/src/cli/illuminator plugins  # 应能看到 memory_usage
```

---

## 7. 测试体系导航

### 测试结构

所有测试遵循**就近放置**原则：

```
src/core/memory/
├── arena.h            ← 被测代码
├── lock_free_queue.h  ← 被测代码
└── test/
    ├── BUILD          ← 测试构建规则
    ├── arena_test.cc
    └── lock_free_queue_test.cc
```

### 运行指南

```bash
# 全量测试（25 个目标）
bazel test //src/...

# 单模块测试
bazel test //src/core/engine/test:all

# 详细输出
bazel test //src/core/engine/test:pipeline_integration_test --test_output=all

# Sanitizer 测试
bazel test //src/... --config=asan  # 内存错误
bazel test //src/... --config=tsan  # 数据竞争
```

### 当前覆盖范围

| 模块 | 测试数 | 状态 |
|------|--------|------|
| Core Infra (Status, Config, Arena, Queue, Pool, Timer, Channel, Batch) | 10 | ✅ |
| Pipeline 集成测试 | 1 | ✅ |
| Processors (4 个) | 4 | ✅ |
| Aggregators (1 个) | 1 | ✅ |
| Sinks (7 个) | 7 | ✅ |
| Source 插件 (依赖 eBPF) | 0 | ❌ 需要 BPF mock |
| 前端 | 0 | ❌ 需引入 Vitest |

---

## 8. 常见问题与陷阱

### Q1: 为什么插件必须设置 `alwayslink = True`？

因为 `IL_REGISTER_*` 宏会生成静态全局变量，在程序 `main()` 之前自动执行注册。如果没有 `alwayslink`，链接器会因为"没有人引用这个符号"而将整个编译单元丢弃，导致插件消失。

### Q2: 为什么 LockFreeQueue 要求容量是 2 的幂？

因为取模运算 `pos % capacity` 被优化为位掩码 `pos & (capacity - 1)`，速度快一个数量级。构造函数会自动向上取整到最近的 2 的幂。

### Q3: Push Source 和 Pull Source 该选哪种？

| 维度 | Pull | Push |
|------|------|------|
| 实时性 | 采集间隔内有延迟 | 事件发生即推送 |
| 复杂度 | 简单（实现 `Collect()`） | 复杂（需管理回调和线程安全） |
| 典型场景 | `/proc` 文件、系统指标 | eBPF ring buffer、事件流 |

### Q4: DataBatch 中的字符串为什么用 `string_view`？

零拷贝设计。字符串实际存储在 Arena 中，`string_view` 只是指针+长度的"视图"。整个管道中不会发生字符串拷贝，直到最终序列化输出（JSON/Prometheus/pprof）时才复制。

### Q5: 为什么有些 `.so` 功能标记为"TODO"？

`SoLoader` 已实现 dlopen/dlsym 加载和 ABI 版本校验，但还缺少从 `IlPluginDescriptor` 到 `PluginRegistry` 的桥接——即加载了 `.so` 但还没有把它注册为可用的 Source/Processor 等。这是 P0 待完成项。

---

## 9. 未来展望与开发路线图

### 近期（1-2 个月）

#### 9.1 安全加固
- **HTTP 认证**：为 API 添加 Token/BasicAuth 认证层
- **绑定地址**：默认改为 `127.0.0.1`，避免意外暴露
- **TLS 支持**：HTTP + WebSocket 加密传输

#### 9.2 存储层完善
- **SQLite Query 修复**：`Query()` 和 `ListProfiles()` 目前返回空结果
- **存储后端扩展**：考虑 ClickHouse/TimescaleDB 后端用于大规模部署
- **数据保留策略**：自动清理过期数据

#### 9.3 .so 插件完整打通
- **SoLoader → Registry 桥接**：加载 `.so` 后自动注册到 PluginRegistry
- **插件热加载**：运行时加载/卸载插件而无需重启
- **WASM 插件**：实现 WASM Runtime 支持多语言插件（Rust/Go/TinyGo）

### 中期（3-6 个月）

#### 9.4 可观测性增强
- **分布式追踪**：OTLP 完整实现（目前是 stub），对接 Jaeger/Tempo
- **日志采集**：添加 journald/文件日志 Source
- **关联分析**：Metrics ↔ Traces ↔ Logs 三大支柱关联

#### 9.5 性能与扩展
- **多节点聚合**：中心化 collector 聚合多个 Illuminator 实例数据
- **远程写入**：Prometheus Remote Write、VictoriaMetrics 兼容
- **GPU 监控**：NVIDIA GPU 利用率/显存/温度采集

#### 9.6 工程质量
- **前端测试**：引入 Vitest + React Testing Library
- **E2E 测试**：Playwright 端到端浏览器测试
- **代码覆盖率**：配置 `bazel coverage` + lcov 报告
- **clang-tidy**：CI 中集成 C++ 静态分析
- **Dockerfile**：多阶段构建容器镜像

### 长期（6-12 个月）

#### 9.7 产品化
- **告警引擎**：基于规则/阈值的告警（类似 Prometheus AlertManager）
- **Dashboard 自定义**：用户可配置的仪表盘布局
- **数据回放**：从存储中回放历史数据，支持 Profile 对比分析
- **移动端适配**：响应式 Web UI

#### 9.8 生态集成
- **Grafana 数据源插件**：作为 Grafana 数据源被引用
- **Kubernetes Operator**：K8s 原生部署和自动发现
- **OpenTelemetry Collector**：作为 OTel Collector 的 receiver/exporter

#### 9.9 内核侧增强
- **Memory Profiling**：内存分配追踪（malloc/free pairing）
- **Futex 分析**：锁竞争检测
- **Cgroup 感知**：容器级资源隔离采集
- **eBPF CO-RE 兼容矩阵**：自动适配不同内核版本

---

## 附录 A: 架构层次与依赖关系图

```
┌─────────────────────────────────────────────────────────┐
│                     CLI (main.cc)                        │
│  daemon | collect | top | version | plugins | storage    │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Server Layer                                 │
│  HttpServer ──── api_routes.h ──── WebSocketManager       │
│  (cpp-httplib)   (REST API)        (WS 推送)              │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Engine Layer                                 │
│  PipelineController                                       │
│    ├── TimerWheel (timerfd+epoll)                         │
│    ├── CollectPool (ThreadPool)                           │
│    ├── Pipeline[] (AsyncChannel + ProcessThread)          │
│    └── SinkPool (ThreadPool)                              │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Plugin Layer                                 │
│  Sources (10) | Processors (4) | Aggregators (1) | Sinks (7) │
│  PluginRegistry (宏注册) | SoLoader (.so 动态加载)        │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Infrastructure Layer                         │
│  Arena | LockFreeQueue | Status/StatusOr | ConfigValue    │
│  spdlog | InternalMetrics | ResourceLimiter               │
│  StorageBackend (SQLite) | JSON Serializer                │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              eBPF Subsystem                               │
│  BpfProgramManager | FeatureProbe | StackTraceUtil        │
│  BPF Probes (cpu/sched/io/net/memory)                     │
│  vmlinux.h | event_types.h (内核/用户态共享)              │
└───────────────────────────────────────────────────────────┘
```

## 附录 B: 关键文件速查表

| 你想了解... | 去看... |
|------------|--------|
| 程序入口 | `src/cli/main.cc` |
| 数据长什么样 | `src/core/engine/data_batch.h` |
| 管道怎么工作 | `src/core/engine/pipeline_controller.h` + `.cc` |
| 配置怎么解析 | `src/core/config/yaml_config_loader.h` |
| 插件怎么注册 | `src/plugin/manager/plugin_registry.h` |
| API 有哪些 | `src/server/api_routes.h` |
| 如何写 Source | `src/sources/cpu/cpu_utilization/cpu_utilization.h` |
| 如何写 Processor | `src/processors/filter/filter_processor.h` |
| 如何写 Sink | `src/sinks/console_output/console_sink.h` |
| 错误怎么处理 | `src/core/common/status.h` |
| 内存怎么管理 | `src/core/memory/arena.h` |
| 线程怎么调度 | `src/core/engine/timer_wheel.h` |
| eBPF 怎么加载 | `src/ebpf/loader/bpf_program_manager.h` |
| 数据怎么序列化 | `src/serialization/json_serializer.h` |
| 数据怎么存储 | `src/storage/sqlite_backend/sqlite_backend.h` |
| 审计报告 | `docs/architecture_audit_v3.md` |
| 设计文档 | `docs/pipeline_v3_design.md` |
| 完整配置参考 | `illuminator.yaml.example` |

---

> **最后建议**：从 `main.cc` 的 `RunDaemon()` 开始，跟踪一次 CPU 采集的完整数据流——从 YAML 配置解析、插件创建、TimerWheel 触发、CollectPool 执行、AsyncChannel 传输、ProcessThread 处理、到 Sink 输出——就能理解整个系统的运转方式。
