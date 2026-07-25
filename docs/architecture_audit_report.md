# Illuminator 全面架构审计报告

> **日期**: 2026-07-25
> **范围**: 后端核心引擎、插件系统、Server 层、前端架构、目录结构与文档
> **更新**: 2026-07-25 — 同步 P0/P1 修复状态

---

## 总评

| 模块 | 评分 | 一句话评价 |
|------|------|----------|
| 核心引擎层 | **A-** | Pipeline v3 设计成熟；sink 线程安全、关机顺序、channel 配置传递已修复 |
| 插件系统 | **B+** | EbpfSourceBase 统一 eBPF 插件；双注册为有意设计，C ABI 仍不成熟 |
| Server 层 | **B-** | 控制面/数据面分离好；v2 路由已提取；Auth+SSE 部分修复（`?token=`） |
| 前端架构 | **B** | SSE 数据流设计出色；页面模式不统一，存在死代码，可访问性差 |
| 目录结构 | **B+** | 实际结构合理；README 与 docs 已同步更新 |
| **综合** | **B+** | 核心架构优秀，P0 正确性问题大部分已修复，剩余 v1/v2 API 统一等待办 |

---

## 一、核心发现（按优先级排序）

### P0 — 必须修复（正确性/安全问题）

| # | 问题 | 模块 | 影响 | 状态 |
|---|------|------|------|------|
| 1 | **Pipeline sink 列表线程安全**: `GetSinks()` 和 `Reconfigure()` 未加锁，与 `AddSinkRuntime` 竞态 | 引擎 | 数据竞争，可能崩溃 | ✅ **已修复** — `shared_mutex` 保护 sinks |
| 2 | **Auth 与 SSE 不兼容**: 启用 `auth_token` 后浏览器 EventSource 无法发送 Authorization header | Server | 启用认证即破坏实时数据 | ⚠️ **部分修复** — 支持 `?token=` 查询参数 |
| 3 | **`/api/v1/query` 和 `/api/v1/budget` 端点缺失**: 前端调用但后端未注册 | Server | QueryConsole 页面无法工作 | 待实现 |
| 4 | **`CpuUtilizationDriver::SetConfig` 忽略 JSON 输入**: 传入的配置被丢弃 | 插件 | 前端配置修改无效果 | ✅ **已修复** — 解析 JSON 并 Reconfigure |
| 5 | ~~**README.md ~30% 准确度**~~ | 文档 | 已修复：目录结构、死链接、SSE 端口 | ✅ **已修复** |
| 6 | **CollectPool/Timer 关机竞态**: 定时器触发的采集任务可能在 InfrastructureManager::Stop() 销毁池后仍在运行 | 引擎 | 关机时 use-after-free | ✅ **已修复** — 关机顺序与取消机制完善 |

### P1 — 应该修复（架构冗余/一致性）

| # | 问题 | 模块 | 影响 | 状态 |
|---|------|------|------|------|
| 7 | **双注册系统**: PluginRegistry + FeatureRegistry 并行，FeatureDriver 从不使用 PluginRegistry 创建插件 | 插件 | 有意设计：Registry 为工厂/内省，Driver 硬编码组件获编译期类型安全 | 保留（有意设计） |
| 8 | **eBPF 三种实现模式**: 模板(67行) vs 手写(924行)，缺乏统一基类 | 插件 | 维护成本高，bug 需同步三处 | ✅ **已完成** — `EbpfSourceBase` 统一基类 |
| 9 | **v1/v2 API 割裂**: `/api/v1/pipelines` vs `/api/v2/features` 查询同一个 FeatureBus | Server | 术语混乱，无弃用计划 | 待办 |
| 10 | **v2 路由内联在 main.cc (~250行)**: 未提取到独立模块 | Server | 不可测试 | ✅ **已完成** — 提取至 `api_v2_routes.h` |
| 11 | **Push 模式 per-event DataBatch**: 每个事件创建一个 batch，高频场景分配压力大 | 插件 | 性能瓶颈 | 待办 |
| 12 | **前端页面模式不统一**: CPU 用 FeaturePageTemplate，其他页面各自手写 shell | 前端 | 维护负担 | 待办 |
| 13 | **core 层依赖 plugin 层**: `feature_driver.h` 直接 include recording_sink.h | 引擎 | 分层违反 | 待办 |
| 14 | **SchedAnalyzerDriver 元数据错误**: `supports_pull=false` 但默认是 Pull 模式 | 插件 | 前端自动发现误标 | 待办 |
| 15 | **SSE syncSubscription bug**: 始终发送 add，从不 remove，导致服务端过滤泄漏 | 前端 | 退订无效 | 待办 |

### P2 — 建议修复（死代码/清理）

| # | 问题 | 模块 | 影响 | 状态 |
|---|------|------|------|------|
| 16 | **`bpf_program_manager.h` 废弃**: 在 BUILD 中但无处引用 | 死代码 | 占空间，误导 | 待清理 |
| 17 | **`wasm_runtime.h` 空壳**: 不在任何 BUILD target 中 | 死代码 | 增加审计范围 | ⚠️ **已标注 STUB** |
| 18 | **`ebpf_sched_tracer` 孤儿插件**: 注册了但无 FeatureDriver，与 sched_analyzer 重叠 | 死代码 | 混乱 | 待清理 |
| 19 | **`CpuStatsAggregator` 无 Feature 集成**: 注册了但无 Driver 使用 | 死代码 | 代码质量误导 | ⚠️ **已标注** — PluginRegistry 可用，未接入 FeatureDriver |
| 20 | **`SinkFanout` 未链接到生产二进制**: 有 BUILD 但 builtin_plugins.cc 不 include | 死代码 | 运行时不可用 | ⚠️ **已标注** — 保留供测试与未来接入 |
| 21 | **`mem_tracer.bpf.c` 不在 BUILD 中**: 源文件存在但无构建目标 | 死代码 | 文件孤儿 | 待清理 |
| 22 | **前端死代码**: `FeatureConfigPanel`、`DataEmptyState`、`ConnectionIndicator`、`aggregationWorker`、`useProfileData` 未使用 | 前端 | 代码膨胀 | 待清理 |
| 23 | **`illuminator.yaml.example` 无效配置键**: `pipelines`、`websocket`、`storage.retention` 等 | 配置 | 误导运维 | ✅ **部分修复** — `global.auto_start`、`global.data_dir`、`engine.channel.*` 已生效 |
| 24 | ~~**7 个引用文档不存在**~~ | 文档 | 已修复：README 文档索引仅保留现有文件 | ✅ **已修复** |
| 25 | **OtlpExportSink 是空壳**: 标记 `IsStub()=true`，无实际 HTTP POST | 插件 | 功能表述不实 | ⚠️ **已标注 STUB** |

---

## 二、模块详细分析

### 1. 核心引擎层 (A-)

**优势**:
- Pipeline v3 事件驱动设计成熟，AsyncChannel 锁无关 + 反压做得好
- InfrastructureManager 作为共享基础设施（TimerWheel + CollectPool + SinkPool）定位清晰
- FeatureDriver 的 Linux 驱动模型风格（Probe/Remove/Pause/Resume）是项目最大亮点
- 测试覆盖率高（34个Bazel测试目标），包含压力测试和并发测试

**已修复**:
- ✅ Pipeline sink 列表 `shared_mutex` 线程安全
- ✅ CollectPool/Timer 关机竞态
- ✅ `EngineConfig.channel` 配置传递给 Pipeline
- ✅ 配置统一为 `EngineConfig`（`InfrastructureConfig` 已移除）

**剩余问题**:
- `PidManager` 存在 O(n²) 去重和全量 BPF map 重写的性能问题
- `ThreadPool` 任务队列无上限
- `self_observability.h` 的 CPU/disk 限制字段声明了但从未执行

### 2. 插件系统 (B+)

**优势**:
- 四阶段模型（Source→Processor→Aggregator→Sink）设计合理
- `IL_REGISTER_*` 宏的静态注册机制优雅高效
- `EbpfSourceBase` 统一 eBPF 插件基类，消除三种实现模式
- Processor 实现一致性最好（passthrough/filter/symbolizer/merger）

**问题**:
- FeatureDriver 从不通过 PluginRegistry 解析插件，直接硬编码具体类型
- C ABI `IlPluginDescriptor` 不成熟：`create(nullptr)` 不传配置，`destroy()` 从未调用，`process()` 字段从未使用
- `.so` 插件无法成为 Feature（缺少桥接）
- 多数 Sink（console/file/prometheus/pprof/otlp）仅在 Registry 中注册，无 Feature 集成

### 3. Server 层 (B-)

**优势**:
- 控制面（REST /api/v2/features）vs 数据面（SSE /api/v1/events）分离清晰
- SSE 实现完善：64KB 帧分割、Last-Event-ID 重放、15s 心跳、动态订阅更新
- SQLite 存储后端能力完整（WAL、批量事务、预编译语句、只读查询守卫）
- v2 路由已提取至 `api_v2_routes.h`，可独立测试

**已修复**:
- ✅ v2 路由模块化
- ⚠️ Auth+SSE：`?token=` 查询参数支持（EventSource 仍无法发 Authorization header）

**剩余问题**:
- 前端调用的 `/api/v1/query` 和 `/api/v1/budget` 后端未实现
- SSE 背压静默丢弃（outbox >= 256 时无指标/日志）
- 录制 API 字段名前后端不一致（`output_dir` vs `file_path` vs `file`）
- JSON 序列化在 SseSink 和 json_serializer.h 中重复实现

### 4. 前端架构 (B)

**优势**:
- SSE 数据流（SseLink → DataBus → Hooks → Pages）是全栈最强的部分
- DataBus 的 100ms 批处理 + rAF + Page Visibility 优化做得好
- DataSource 抽象统一了 Live/Replay
- 168 个单元测试，服务层覆盖率高

**问题**:
- 只有 CpuPage 使用 FeaturePageTemplate，其他4个指标页面各自手写 shell
- 三层缓冲（DataBus ring → useDataSource buffer → hook TimeSeriesBuffer）不统一
- 多个组件/hook 未使用（FeatureConfigPanel, aggregationWorker, useProfileData 等）
- Replay 功能不完整（只有 CPU 和 Memory，IO/Network/GPU 只是占位）
- 无 React.memo 优化，图表组件随父组件全量重渲染
- 可访问性差：无 ARIA 标签、焦点管理、减速动画支持

### 5. 目录结构与文档 (B+)

**优势**:
- `src/` 目录结构合理：core/ebpf_common/plugin/server/cli 分层清晰
- `docs/architecture_overview.md` ~95% 准确
- `docs/onboarding_guide.md` ~85% 准确
- CI 配置合理（后端+BPF+前端+Sanitizer）

**已修复**:
- ✅ `illuminator.yaml.example` — `global.auto_start`、`global.data_dir`、`engine.channel.*` 已生效
- ✅ README.md 文档同步（移除 pipelines YAML 示例、更新路径）
- ~~README.md 文档负债~~（已修复：目录结构、死链接、SSE 端口）
- ~~`check_env.sh` 检查 9528 端口~~（已修复：仅检查 9527）
- ~~`Makefile` 的 `make dev` 引用不存在的 `illuminator.yaml`~~（已修复：改用 `illuminator.yaml.example`）
- ~~`Dockerfile` EXPOSE 9528~~（已修复：仅 EXPOSE 9527）

**剩余问题**:
- `illuminator.yaml.example` 中 `pipelines`、`websocket` 等键仍无效

---

## 三、架构冗余总结

| 冗余点 | 涉及文件 | 建议 | 状态 |
|--------|---------|------|------|
| PluginRegistry + FeatureRegistry 双注册 | `plugin_registry.h`, `feature_registry.h` | 有意分离：Registry 为工厂/内省，FeatureRegistry 为 Feature 编排；Driver 硬编码组件类型以获编译期安全 | 保留 |
| ~~EngineConfig + InfrastructureConfig 重复~~ | `config.h`, `infrastructure_manager.h` | 统一为单一配置源 | ✅ **已修复** — 统一为 `EngineConfig` |
| DriverInfo + FeatureStats 重叠 | `feature_driver.h` | 合并查询 DTO | 待办 |
| Pipeline::RunProcessors() + HandleData() 处理器循环 | `pipeline.h:286-294, 371-379` | 提取共享辅助函数 | 待办 |
| SseSink 序列化 vs json_serializer.h | `sse_handler.h`, `json_serializer.h` | 统一为 `SerializeFeatureBatch()` | 待办 |
| 前端 useMetricsData/History vs useDataSource | hooks/ | 弃用旧 hook | 待办 |
| 前端三层缓冲 | dataBus + hooks + TimeSeriesBuffer | 统一为 DataBus ring + hook state | 待办 |

---

## 四、性能关注点

| 问题 | 影响 | 建议 |
|------|------|------|
| Push 模式每事件一个 DataBatch | 高频场景大量分配 | 定时批量排空 ring buffer |
| SinkPool N 个 sink × N 个任务 per batch | 高 sink 数 × 高吞吐 = 任务爆炸 | 批量 sink 提交 |
| PidManager O(n²) 去重 + 全量 map 重写 | 超过百级 PID 性能差 | unordered_set + 增量更新 |
| InternalMetrics 全局锁 | 多 Pipeline 竞争 | 按 pipeline 分片或用原子操作 |
| ThreadPool 无上限队列 | 持续过载内存增长 | 可选最大队列长度 |
| 前端无 React.memo | 图表组件全量重渲染 | 添加 memo + 稳定 props |
| ECharts 662KB chunk | 首个图表页面加载慢 | 按需加载或进一步拆分 |

---

## 五、扩展性评估

| 扩展场景 | 可行性 | 障碍 |
|---------|--------|------|
| 新增 C++ Source + FeatureDriver | ✅ 简单 | 需同时写两个文件（Source + Driver） |
| 新增 Processor/Sink | ⚠️ 注册成功但无 Feature 使用 | Driver 硬编码组件 |
| 外部 .so 插件 | ❌ 不可用 | C ABI 不成熟，无法成为 Feature |
| WASM 插件 | ❌ 空壳（已标注 STUB） | 未实现 |
| 前端新增 Feature 页面 | ⚠️ 需大量样板 | 无统一页面模板 |
| 运行时动态组合 Pipeline | ❌ 已移除 | YAML pipelines 不再运行；改由 /api/v2/features 控制 |
| 跨平台支持 | ❌ 纯 Linux | timerfd/epoll/eBPF 深度绑定 |

---

## 六、Top 10 行动建议

| 优先级 | 行动 | 预期效果 | 状态 |
|--------|------|---------|------|
| P0-1 | **修复 Pipeline sink 列表线程安全** | 消除数据竞争 | ✅ 已修复 |
| P0-2 | **重写 README.md** | 准确描述当前架构 | ✅ 已修复 |
| P0-3 | **实现或删除 `/api/v1/query`** | QueryConsole 可用 | 待办 |
| P0-4 | **解决 Auth+SSE 不兼容** | 认证可真正使用 | ⚠️ 部分修复（`?token=`） |
| P1-1 | **统一 eBPF 插件基类**（参见 ebpf_plugin_redesign.md） | 消除三种实现模式 | ✅ 已完成 |
| P1-2 | **提取 v2 路由到独立模块** | 可测试性 | ✅ 已完成 |
| P1-3 | **Push 模式批量排空 ring buffer** | 性能提升 + 消除 poll 线程 | 待办 |
| P1-4 | **统一前端页面模板** | 减少页面维护负担 | 待办 |
| P2-1 | **清理死代码**（wasm/bpf_program_manager/fanout/sched_tracer） | 代码干净度 | ⚠️ 部分完成（STUB 标注） |
| P2-2 | **清理 YAML 配置**（删除无效键或实现它们） | 运维不被误导 | ⚠️ 部分完成（auto_start/data_dir/channel 已生效） |
