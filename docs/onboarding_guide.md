# Illuminator 项目入门指南

> **文档目的**：为首次接触本项目的开发者提供系统性的学习路径，帮助快速理解项目架构、核心概念和代码组织。
>
> **适用人群**：C++ 后端开发者、前端开发者、系统工程师、对 eBPF/可观测性感兴趣的开发者
>
> **预计阅读时间**：30 分钟精读 + 2-3 天实操探索
>
> **更新日期**：2026-06-16

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

Illuminator 是一个 **高性能、插件化的全栈可观测性平台**。它通过 eBPF 技术在 Linux 上进行零侵入数据采集（CPU、调度器、I/O、网络），配套自研 Web 可视化平台实时展示，支持 WebSocket 实时推送 + HTTP 降级双通道。

**一句话总结**：`eBPF 数据采集 → 异步管道处理 → WS 实时推送/REST API → React 可视化`

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
| WebSocket | 自定义 RFC6455 实现 | 无外部依赖、Bearer Auth 集成 |
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

# 启动后端守护进程（需要 root，因为 eBPF）
sudo ./bazel-bin/src/cli/illuminator daemon

# 另一个终端：启动前端开发服务器
cd web && npm run dev    # → http://localhost:3000
```

### 2.3 验证

```bash
# 后端健康检查
curl http://localhost:9527/healthz

# 查看已注册的 Feature（管道）
curl http://localhost:9527/api/v1/features

# 启动 CPU 监控
curl -X POST http://localhost:9527/api/v1/features/cpu_utilization/start -d '{}'

# 获取实时数据
curl http://localhost:9527/api/v1/features/cpu_utilization/collect | python3 -m json.tool
```

### 2.4 访问界面

| 地址 | 说明 |
|------|------|
| `http://localhost:3000` | 前端开发服务器（Vite HMR，代理到后端） |
| `http://localhost:9527` | 后端直接访问（静态文件 + API） |
| `http://localhost:9527/healthz` | 健康检查（无认证） |
| `http://localhost:9527/metrics` | Prometheus 指标（无认证） |
| `ws://localhost:9528/ws/features` | WebSocket 实时推送端点 |

---

## 3. 后端学习路线图

### 阶段一：理解数据流（Day 1）

**目标**：理解"数据从哪来、到哪去"

```
Source (数据源) → AsyncChannel → Processor (处理器) → Aggregator (聚合器) → Sink (输出端)
```

**必读文件**：
1. `src/core/engine/data_batch.h` — 数据的"容器"长什么样
2. `src/plugin/api/source_plugin.h` — Pull 模式 vs Push 模式
3. `src/plugin/api/sink_plugin.h` — 数据最终去向

**实验**：
```bash
# 启动后，触发一次 CPU 采集
curl -X POST http://localhost:9527/api/v1/features/cpu_utilization/start -d '{}'
sleep 2
curl http://localhost:9527/api/v1/features/cpu_utilization/collect | python3 -m json.tool
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
2. `src/core/engine/pipeline_controller.h` — `Pipeline` 类的 `Start()` 和 `ProcessLoop()`
3. `src/core/engine/pipeline_controller.cc` — `BuildFromConfig()` 和 `StartAll()` 编排逻辑

### 阶段三：理解 FeatureManager（Day 2）

**目标**：理解 Feature 生命周期管理和前后端交互的核心

```
FeatureManager (高层抽象)
├── 接收 HTTP API 请求 (start/stop/pause/resume/reconfigure)
├── 按需创建独立 Pipeline 实例
├── 自动注入 3 种运行时 Sink:
│   ├── StreamSink → StreamSinkStore (供 /collect 和 /stream API 查询)
│   ├── WebSocketSink → WebSocketSinkStore (供 WS 广播推送)
│   └── RecordingSink (录制时按需注入)
└── 注册定时器到共享 TimerWheel
```

**必读文件**：
1. `src/core/engine/feature_manager.h` — Feature 状态机、生命周期管理
2. `src/server/api_routes.h` — REST API 全景
3. `src/server/websocket_manager.h` — WS 推送 + 认证逻辑

### 阶段四：理解配置系统（Day 2）

**目标**：理解 YAML 如何驱动整个系统

```
illuminator.yaml.example / 内置 kDefaultConfigYaml
    └→ YamlConfigLoader::LoadFromString/File()
        └→ GlobalConfig { engine, server{auth_token}, pipelines[] }
            └→ PipelineController::BuildFromConfig(config)
                └→ PluginRegistry::CreateSource/Processor/Aggregator/Sink
```

**必读文件**：
1. `illuminator.yaml.example` — 完整配置参考
2. `src/core/common/config.h` — `ConfigValue`, `PipelineConfig`, `GlobalConfig` 三层结构
3. `src/core/config/yaml_config_loader.h` — YAML → GlobalConfig 的转换逻辑

### 阶段五：理解插件系统（Day 2-3）

**必读文件**：
1. `src/plugin/manager/plugin_registry.h` — 宏注册机制 `IL_REGISTER_*`
2. `src/sources/cpu/cpu_utilization/cpu_utilization.h` — 最简单的 Pull Source
3. `src/processors/filter/filter_processor.h` — 标签过滤 Processor
4. `src/sinks/console_output/console_sink.h` — 最简单的 Sink

### 阶段六：eBPF 子系统（专项）

**前置知识**：BPF 基础、libbpf 用法

**必读文件**：
1. `src/ebpf/include/event_types.h` — 内核/用户态共享数据结构
2. `src/ebpf/loader/bpf_program_manager.h` — libbpf 封装层
3. `src/sources/ebpf_ring_buffer_source.h` — eBPF Push Source 公共基类
4. `src/ebpf/probes/cpu/cpu_profiler.bpf.c` — BPF C 程序示例

---

## 4. 前端学习路线图

### 阶段一：整体结构（1 小时）

**目标**：理解前端的文件组织和路由

```
web/src/
├── App.tsx                    路由 (10 页面, React.lazy + Suspense)
├── main.tsx                   挂载点
├── components/charts/         图表组件 (ECharts + FlameGraph)
├── hooks/                     数据 hooks (LiveDataSource 驱动)
├── services/                  apiClient + liveDataSource + dataSource 接口
├── stores/                    Zustand 状态 (time/pipeline/annotation)
├── workers/                   Web Worker (火焰图异步计算)
└── pages/                     10 个懒加载页面
```

**必读文件**：
1. `web/src/App.tsx` — 路由、全局布局、键盘快捷键
2. `web/src/services/dataSource.ts` — `DataSource` 接口定义
3. `web/src/services/liveDataSource.ts` — WS+HTTP 双通道实现

### 阶段二：数据流（1 小时）

**目标**：理解前端如何获取和消费后端数据

```
                      ┌─ ws://host/ws/features ──────────┐
getDataSource() ─────►│  subscribe:{feature}              │◄── WS 可用
(全局单例)            │  ← JSON 实时推送 (1s)             │
                      └──────────────────────────────────┘
                      ┌─ api.featureCollect(feature) ────┐
                      │  1s HTTP 轮询                     │◄── WS 断开（自动降级）
                      └──────────────────────────────────┘

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
  1. api.featureStream(name, cursor) → 增量拉取样本 (1.5s)
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

### 5.2 LiveDataSource — 前端数据供应核心

```typescript
LiveDataSource (全局单例)
├── connectWs() → ws://host/ws/features
│   ├── onopen → 停止所有 HTTP 轮询，发送 subscribe:{feature}
│   ├── onmessage → 解析 JSON，分发给对应 feature 的回调
│   └── onclose → 启动 HTTP 轮询降级，指数退避重连
├── subscribe(feature, callback) → 注册回调
│   ├── WS 已连接 → 直接发送 subscribe:{feature}
│   └── WS 未连接 → 启动该 feature 的 HTTP 轮询
├── startPolling(feature) → api.featureCollect(feature) 每 1s
├── setupVisibility() → 页面隐藏时停止轮询/推送，可见时恢复
└── scheduleReconnect() → 指数退避 (1s → 2s → 4s → ... → 30s max)
```

### 5.3 Feature 生命周期

```
前端操作               →   HTTP API              →   后端状态
─────────────────────────────────────────────────────────────
页面进入 CPU 标签页    →   POST /features/cpu_utilization/start
                       →   FeatureManager::Start() → Pipeline 启动
                       →   TimerWheel 注册 1s 采集定时器
                       →   Source::Collect() 开始周期执行
                       →   数据流入 StreamSink + WebSocketSink

页面切到 Memory 标签页 →   POST /features/cpu_utilization/stop
                       →   FeatureManager::Stop() → Pipeline 停止
                       →   POST /features/memory_utilization/start
                       →   新管道启动...

火焰图设置 PID         →   POST /features/cpu_profile/reconfigure
                       →   {target_pids: [1234]}
                       →   Source 更新 BPF 过滤器
                       →   清空 StreamSinkStore + BPF maps
```

### 5.4 AsyncChannel — 异步通信

| 消息类型 | 用途 | 来源 |
|---------|------|------|
| `DataBatchPtr` | 正常数据批次 | Source::Collect() 或 Push callback |
| `FlushSentinel` | 触发 Aggregator flush | TimerWheel 周期注入 |

**反压机制**：队列使用率 > 80% 触发反压，< 20% 解除。
**丢弃策略**：`drop_newest`（保留历史）或 `drop_oldest`（保留最新）。

### 5.5 WebSocket 认证

后端 WS 端口 (默认 9528) 在 Upgrade 握手阶段验证 token：
- `Authorization: Bearer <token>` 请求头
- `?token=<token>` URL 查询参数（浏览器 WS API 备选）
- 配置中 `server.auth_token` 为空时跳过验证（开发模式）

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
| 8 | `src/core/engine/data_batch.h` | `Record`/`StackSample`/`DataBatch` + Arena |

### 第三轮：管道引擎（3 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 9 | `src/core/memory/arena.h` | bump-pointer 分块策略 |
| 10 | `src/core/memory/lock_free_queue.h` | MPSC CAS 环形缓冲区 |
| 11 | `src/core/engine/async_channel.h` | ChannelItem variant、反压水位线 |
| 12 | `src/core/engine/timer_wheel.h` | timerfd + epoll + eventfd |
| 13 | `src/core/engine/pipeline_controller.h` | Pipeline 生命周期 |
| 14 | `src/core/engine/feature_manager.h` | Feature 状态机、Sink 注入 |

### 第四轮：插件 & 服务层（2 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 15 | `src/plugin/manager/plugin_registry.h` | IL_REGISTER_* 宏注册 |
| 16 | `src/sources/cpu/cpu_utilization/cpu_utilization.h` | 典型 Pull Source |
| 17 | `src/sinks/stream_sink/stream_sink.h` | StreamSinkStore cursor 协议 |
| 18 | `src/server/api_routes.h` | 39 个 REST 端点全景 |
| 19 | `src/server/websocket_manager.h` | WS 订阅模型 + 认证 + 广播 |

### 第五轮：前端架构（2 小时）

| 序号 | 文件 | 阅读重点 |
|------|------|---------|
| 20 | `web/src/App.tsx` | 路由、全局布局 |
| 21 | `web/src/services/liveDataSource.ts` | WS/HTTP 双通道核心 |
| 22 | `web/src/hooks/useDataSource.ts` | 全局单例 + 连接状态 + 页面激活 |
| 23 | `web/src/hooks/useCpuData.ts` | 典型数据 hook（subscribe 模式） |
| 24 | `web/src/services/apiClient.ts` | REST API 客户端全集 |
| 25 | `web/src/components/charts/ProfileSnapshot.tsx` | 火焰图：数据获取+Worker+渲染 |
| 26 | `web/src/workers/flameGraphWorker.ts` | Worker 端：树构建算法 |

---

## 7. 如何新增一个插件（端到端示例）

以添加一个 **MemoryUsageSource**（内存使用监控）为例：

### Step 1: 创建源文件

`src/sources/memory/memory_usage/memory_usage.h`

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

    bool IsPushMode() const override { return false; }
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

在 `src/plugin/builtin/builtin_plugins.cc` 中添加 include：
```cpp
#include "sources/memory/memory_usage/memory_usage.h"
```

在 `src/plugin/BUILD` 的 `builtin` target deps 中添加：
```python
"//src/sources:memory_usage",
```

### Step 4: 添加配置

在 `illuminator.yaml.example` 或 `main.cc` 的 `kDefaultConfigYaml` 中：
```yaml
pipelines:
  memory_monitor:
    source:
      type: memory_usage
      config:
        interval_ms: 2000
    sinks:
      - type: local_storage
        config:
          backend: sqlite
          path: /tmp/illuminator_data
          pipeline: memory_monitor
```

### Step 5: 验证

```bash
bazel build //src/cli:illuminator
sudo ./bazel-bin/src/cli/illuminator plugins  # 应能看到 memory_usage
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
import { usePageActivation } from '../hooks/useDataSource'

export default function MemoryPage() {
  usePageActivation('memory', [
    { name: 'memory_utilization', tier: 1 },
  ])

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

- **Hook 使用 `getDataSource().subscribe()`**：自动获得 WS 推送 + HTTP 降级
- **页面使用 `usePageActivation()`**：进入页面时自动 start feature，离开时可 stop
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
| Services (apiClient, liveDataSource) | ~15 | ✅ |
| Workers (flameGraphWorker) | ~10 | ✅ |
| 组件渲染测试 | 0 | ❌ 待补充 |
| E2E (Playwright) | 0 | ❌ 待补充 |

---

## 10. 常见问题与陷阱

### Q1: 为什么插件必须设置 `alwayslink = True`？

`IL_REGISTER_*` 宏生成静态全局变量，在 `main()` 之前自动注册。没有 `alwayslink`，链接器会丢弃整个编译单元。

### Q2: 为什么 LockFreeQueue 要求容量是 2 的幂？

取模 `pos % capacity` 优化为位掩码 `pos & (capacity - 1)`。构造函数自动向上取整。

### Q3: Push Source 和 Pull Source 该选哪种？

| 维度 | Pull | Push |
|------|------|------|
| 实时性 | 采集间隔内有延迟 | 事件发生即推送 |
| 复杂度 | 简单（实现 `Collect()`） | 复杂（回调+线程安全） |
| 典型场景 | `/proc` 文件、系统指标 | eBPF ring buffer、事件流 |

### Q4: 前端数据 hook 中的 intervalMs 参数有什么用？

目前已是**遗留参数**。hooks 不再自己管理轮询定时器——由 `LiveDataSource` 统一管理（默认 1s）。该参数保留在签名中以保持向后兼容，但不影响实际行为。

### Q5: WebSocket 断开时前端会卡住吗？

不会。`LiveDataSource` 在 WS 断开时自动降级为 HTTP 轮询（`api.featureCollect()`），延迟从 ~50ms 增加到 ~1s，但用户无感知。重连使用指数退避（最大 30s）。

### Q6: ProfileSnapshot 为什么不用 LiveDataSource？

火焰图需要 **cursor-based 增量拉取**（累积历史样本进行树构建），而 LiveDataSource 的 WS 推送只发送最新一批数据（latest-only）。二者语义不同。ProfileSnapshot 使用独立的 `api.featureStream(name, cursor)` 实现增量语义。

### Q7: 如何给后端配置认证？

创建 `illuminator.yaml` 并添加：
```yaml
server:
  http:
    listen: "0.0.0.0:9527"
  auth_token: "your-secret-token"
```

前端需要通过 Vite proxy 或直连时携带 `Authorization: Bearer <token>` 头。WS 连接自动通过 URL 参数 `?token=<token>` 传递。

### Q8: 默认配置中为什么没有 auth_token？

开发模式下不强制认证，方便调试。生产部署应通过配置文件设置 `server.auth_token`。

---

## 11. 未来展望与开发路线图

### ✅ 已完成

- HTTP + WS 统一 Bearer Auth 认证
- WS 实时推送 + HTTP 降级双通道
- 火焰图 Web Worker 异步计算
- 死代码清理 + 依赖精简
- 前端 Vitest 测试 (56 tests)
- SQLite WAL 存储 + 自动 Prune
- .so 插件动态加载 (SoLoader + 热加载 API)

### 近期（P2）

| 项 | 说明 |
|-----|------|
| Replay 流式解析 | `ReadableStream` 替代 `file.text()` 支持大文件回放 |
| 前端组件测试 | React Testing Library 补充组件渲染测试 |
| E2E 测试 | Playwright 端到端浏览器测试 |
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
| 数据长什么样 | `src/core/engine/data_batch.h` |
| 管道怎么工作 | `src/core/engine/pipeline_controller.h` + `.cc` |
| Feature 管理 | `src/core/engine/feature_manager.h` |
| 配置怎么解析 | `src/core/config/yaml_config_loader.h` |
| API 有哪些 | `src/server/api_routes.h` |
| WS 推送怎么做 | `src/server/websocket_manager.h` |
| 如何写 Source | `src/sources/cpu/cpu_utilization/cpu_utilization.h` |
| 如何写 Processor | `src/processors/filter/filter_processor.h` |
| 如何写 Sink | `src/sinks/console_output/console_sink.h` |
| 错误怎么处理 | `src/core/common/status.h` |
| 内存怎么管理 | `src/core/memory/arena.h` |
| 线程怎么调度 | `src/core/engine/timer_wheel.h` |
| eBPF 怎么加载 | `src/ebpf/loader/bpf_program_manager.h` |
| 前端数据流 | `web/src/services/liveDataSource.ts` |
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
│  Pages → Hooks → LiveDataSource (WS ↔ HTTP fallback)    │
│  ProfileSnapshot → Worker → FlameGraph div rendering    │
│  ECharts 6 (tree-shaken) for time-series charts         │
└──────────────────────────┬──────────────────────────────┘
                           │ REST :9527 / WS :9528
┌──────────────────────────▼──────────────────────────────┐
│                     CLI (main.cc)                        │
│  daemon | collect | top | version | plugins | storage   │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Server Layer                                │
│  HttpServer (cpp-httplib) + API Routes (39 endpoints)   │
│  WebSocketManager (custom RFC6455, Bearer Auth)         │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Engine Layer                                │
│  FeatureManager (lifecycle) + PipelineController (exec) │
│    ├── TimerWheel (timerfd+epoll)                       │
│    ├── CollectPool (ThreadPool)                         │
│    ├── Pipeline[] (AsyncChannel + ProcessThread)        │
│    └── SinkPool (ThreadPool)                            │
└──────────────┬──────────────────────────────────────────┘
               │
┌──────────────▼──────────────────────────────────────────┐
│              Plugin Layer                                │
│  Sources (9) | Processors (4) | Aggregators (1) | Sinks (9) │
│  PluginRegistry (macro) | SoLoader (.so) | WASM (stub) │
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
│  BpfProgramManager | FeatureProbe | StackTraceUtil      │
│  7 probes: cpu_profiler/sampler, offcpu, sched(×2),     │
│            bio_latency, net_tracer                       │
│  vmlinux.h | event_types.h (kernel/userspace shared)    │
└─────────────────────────────────────────────────────────┘
```

---

> **最后建议**：从 `main.cc` 的 `RunDaemon()` 开始，跟踪一次 CPU 采集的完整数据流——从配置解析、FeatureManager 注册、HTTP API 触发 start、TimerWheel 调度、CollectPool 执行、AsyncChannel 传输、ProcessThread 处理、到 WebSocketSink 广播给前端——就能理解整个系统的运转方式。
