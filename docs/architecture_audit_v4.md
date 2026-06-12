# Illuminator 架构审计 v4

> **审计日期**: 2026-06-10  
> **代码基线**: `infra_optimize` 分支  
> **覆盖范围**: 后端 C++20 + 前端 React/TS + CI/CD + Dockerfile + 插件体系

---

## 一、架构概览

```
┌───────────────────────────────────────────────────────────────┐
│                    Illuminator v3 Architecture                  │
├───────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────┐    ┌────────────┐    ┌───────────┐    ┌──────┐ │
│  │ Sources  │───▶│ Processors │───▶│Aggregators│───▶│Sinks │ │
│  └──────────┘    └────────────┘    └───────────┘    └──────┘ │
│       │                                                   │     │
│       │ Push/Pull                              Write (pool)│     │
│       ▼                                                   ▼     │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │           PipelineController (v3 event-driven)            │ │
│  │  TimerWheel(1) → CollectPool(M) → ProcessThread(1/pipe) │ │
│  │                                  → SinkPool(K)            │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                                 │
│  ┌───────────┐  ┌────────────┐  ┌─────────────────────────┐  │
│  │ HTTP API  │  │ WebSocket  │  │   Storage (SQLite WAL)   │  │
│  │ :9527     │  │ Push/Pull  │  │   + Prune + Retention    │  │
│  └───────────┘  └────────────┘  └─────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

## 二、核心组件状态

| 组件 | 文件 | 状态 | 备注 |
|------|------|------|------|
| TimerWheel | `core/engine/timer_wheel.h` | ✅ 完成 | timerfd+epoll+eventfd |
| CollectPool | `core/threading/thread_pool.h` | ✅ 完成 | 并行 Collect |
| ProcessThread | `core/engine/pipeline_controller.h` | ✅ 完成 | variant dispatch |
| SinkPool | `core/threading/thread_pool.h` | ✅ 完成 | 含 256-task 背压 |
| AsyncChannel | `core/engine/async_channel.h` | ✅ 完成 | MPSC lock-free |
| DataBatch | `core/engine/data_batch.h` | ✅ 完成 | Arena + InternString 去重 |
| PluginRegistry | `plugin/manager/plugin_registry.h` | ✅ 完成 | 含 .so 动态加载桥接 |
| StorageBackend | `storage/storage_backend.h` | ✅ 完成 | SQLite WAL + Prune |
| HTTP Server | `server/api_routes.h` | ✅ 完成 | Bearer Auth + /healthz |
| WebSocket Sink | `sinks/websocket_sink/` | ✅ 完成 | 30-batch 缓冲 |
| 版本注入 | `tools/version.bzl` | ✅ 完成 | Bazel stamp + Vite define |

## 三、问题追踪

### P0 — 严重问题（全部已修复 ✅）

| # | 问题 | 修复 |
|---|------|------|
| 1 | 零测试覆盖 | 25 个 cc_test + Vitest 前端 |
| 2 | SQLite 并发死锁 | busy_timeout + write_mutex |
| 3 | Query 反序列化失败 | JSON ↔ Record 完整实现 |
| 4 | HTTP 0.0.0.0 无认证 | 默认 127.0.0.1 + Bearer Token |
| 5 | string_view 悬垂 | Arena 生命周期绑定 DataBatch |
| 6 | .so 插件未桥接 | BridgeDescriptorToRegistry |
| 7 | Raw SQL 注入 | 只允许 SELECT/EXPLAIN/PRAGMA |
| 8 | 内存泄漏 | InternString 去重 + SQLite Prune + SinkPool 背压 |

### P1 — 重要改进（进行中）

| # | 领域 | 问题 | 建议 |
|---|------|------|------|
| 1 | 前端 | `wsManager.subscribe()` 无调用者 | 接入实时数据推送 |
| 2 | 前端 | `useApi.ts` 未使用 | 按需保留或移除 |
| 3 | 存储 | retention 硬编码 30min | 接入 YAML 配置 |
| 4 | eBPF | CO-RE 兼容性未测试 | 添加内核版本检测 |
| 5 | 安全 | Auth Token 明文配置 | 环境变量注入 |

### P2 — 未来优化

| # | 领域 | 方向 |
|---|------|------|
| 1 | 插件 | WASM sandbox runtime |
| 2 | 存储 | RocksDB/InfluxDB 后端 |
| 3 | 分布式 | 多节点聚合 |
| 4 | 前端 | WebSocket 实时图表 |
| 5 | CI | 性能回归测试 |

## 四、测试覆盖

### 后端（25 个 cc_test 目标）

| 层 | 目标 | 用例数 |
|----|------|--------|
| core/common | config, status | 18 |
| core/engine | data_batch, async_channel, timer_wheel, pipeline_integration | 62 |
| core/memory | arena, lock_free_queue | 15 |
| core/threading | thread_pool | 8 |
| storage | sqlite_backend | 10 |
| sinks (7) | console, file, local_storage, otlp, pprof, prometheus, websocket | 35 |
| processors (3) | filter, passthrough, stack_symbolizer, stack_merger | 20 |
| aggregators | cpu_stats_aggregator | 6 |
| plugin | plugin_registry | 5 |
| server | server_test | 6 |
| sources | cpu_utilization | 5 |

### 前端（Vitest + ESLint）

| 文件 | 用例 |
|------|------|
| `usePolling.test.ts` | 2 |
| `useTimeStore.test.ts` | 5 |
| ESLint (max-warnings=100) | ✅ 通过 |

## 五、CI/CD 流程

```
push/PR → backend-build → backend-test (25 targets)
        → frontend-build → tsc → eslint → vitest → vite build
        → ci-gate (all green)
        
workflow_dispatch → docker-build (多阶段, GIT_VERSION 注入)

tag v* → release.yml → verify → docker push → GitHub Release
```

## 六、路线图

```
Phase 1 ✅ 基础质量保障
  ├─ 测试体系（25 后端 + Vitest 前端）
  ├─ CI/CD（GitHub Actions）
  ├─ Dockerfile + 环境检测脚本
  └─ 版本注入（Bazel stamp + Vite）

Phase 2 🔄 稳定性与性能
  ├─ 内存泄漏修复（InternString 去重 + Prune + 背压）
  ├─ offcpu_profiler 无效分配修复
  ├─ 数据保留策略（30min 默认）
  └─ WebSocket 缓冲优化

Phase 3 📋 插件生态
  ├─ 更多 eBPF 探针
  ├─ WASM 插件沙箱
  └─ 外部存储后端
```
