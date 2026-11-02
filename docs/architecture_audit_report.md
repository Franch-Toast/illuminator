# Illuminator 架构审查报告

> **日期**：2026-11-02
> **审查对象**：`architecture_optimize` 分支（commit `8e3363a`）
> **审查范围**：核心引擎、插件系统、数据模型、配置、eBPF 采集、Server 层、存储/录制、构建与测试
> **验证方式**：逐文件阅读源码 + `bazel test //src/...`
> **当前基线**：构建通过，21/21 测试全部 PASSED（仅有 `EngineConfig.channel` 未显式初始化的编译警告）
>
> 本文替代此前 2026-07-25 版本。旧版本中的问题已大量变化，本文以当前源码为准重新审查。

---

## 目录

1. [总体结论](#一总体结论)
2. [P0 — 必须优先修复的正确性问题](#二p0--必须优先修复的正确性问题)
3. [P1 — 架构与扩展性问题](#三p1--架构与扩展性问题)
4. [P2 — 性能与可维护性优化](#四p2--性能与可维护性优化)
5. [目标架构建议](#五目标架构建议)
6. [建议的落地顺序](#六建议的落地顺序)
7. [附录：问题索引](#七附录问题索引)

---

## 一、总体结论

**方向是对的，骨架也基本优秀，但“内核”目前还不足以支撑“后续只加插件”的目标。**

### 已经做好的部分

- Pipeline v3 的 **调度/执行分离**、`AsyncChannel` 的 variant 事件模型、水位线反压（`src/core/engine/async_channel.h`）设计成熟。
- TimerWheel 基于 `timerfd + epoll + eventfd`，无忙等、支持回调外置执行（`src/core/engine/timer_wheel.h`）。
- FeatureDriver 的 Linux 驱动模型（Probe/Remove/Pause/Resume）是项目最有价值的抽象。
- Arena 字符串驻留、零拷贝路由、SSE 分帧与 Last-Event-ID 重放、ASAN/TSAN CI 都已具备。
- 代码注释密度高，模块职责在文档层面描述清晰。
- 21 个后端测试全部通过，覆盖 Pipeline、Channel、TimerWheel、FeatureBus、录制生命周期、压力测试等。

### 模块评分

| 模块 | 评分 | 一句话评价 |
|------|------|-----------|
| 核心引擎层 | **B+** | Pipeline v3 设计成熟，但 Feature 生命周期与 in-flight 任务缺少同步 |
| 插件系统 | **C+** | 组件抽象合理，但 Feature 仍硬编码、C ABI 名不副实、无法真正热卸载 |
| 数据模型 | **B-** | Arena/零拷贝思路好，但数据类型不可扩展，新数据形状需改 core |
| 配置系统 | **C+** | 三条配置路径并存，校验缺失，部分配置不生效 |
| eBPF 采集 | **B** | 统一基类好，但 ring buffer 仍走定时轮询，map 数据存在丢弃 |
| Server 层 | **B-** | 控制面/数据面分离方向正确，但控制面直接触碰数据面，SSE 线程模型有隐患 |
| 存储/录制/回放 | **C** | SQLite 后端完整但未接入生产；录制是 per-feature Sink，无回放引擎 |
| 测试与 CI | **B+** | 测试数量可观，ASAN/TSAN 已接入，但缺少并发生命周期与 ABI 测试 |
| **综合** | **B** | 核心架构优秀，但需要先解决运行时边界问题，才能进入“只加插件”的演进阶段 |

### 四个根因级问题

| 根因 | 表现 | 影响 |
|------|------|------|
| **控制面直接操作数据面** | HTTP 线程直接调用 `Source::Collect()`、`Pipeline::RunProcessors()`、`Reconfigure()` | 数据竞争、状态错乱、Processor/Aggregator 的“独占线程”假设被破坏 |
| **Feature 生命周期没有串行化** | Probe/Remove/Pause/Resume 只靠零散的原子状态判断 | 并发启停、Timer 回调 UAF、Pipeline 指针悬空 |
| **插件边界不是真正的插件边界** | FeatureDriver 硬编码具体 Source 和 SseSink；`.so` C ABI 无法承载 C++ 对象 | 新插件需改 7 处文件并重编译；`.so` 热插拔实际不可用 |
| **数据/会话模型偏薄** | 只有 `Record + StackSample`；录制是 per-feature Sink；无回放引擎 | 新数据类型要改 core；离线回放、跨 Feature 对齐很难做 |

如果这四点不解决，后续每增加一个插件，复杂度都会线性甚至指数上升。

---

## 二、P0 — 必须优先修复的正确性问题

### P0-1 控制面直接调用数据面，绕过 Pipeline 的线程模型

**证据**：

- `src/server/api_routes.h:189`：HTTP 处理函数里直接 `source->Collect()`。
- `src/server/api_routes.h:194`：直接 `drv->GetPipeline()->RunProcessors(...)`。
- `src/core/engine/feature_driver.h:300-305`：`Reconfigure()` 从 HTTP 线程直接调用 `pipeline_->Reconfigure()`。
- `src/core/engine/pipeline.h:330-360`：`Reconfigure()` 在运行期间直接修改 `source_ / processors_ / aggregator_ / sinks_`。

**为什么严重**：

Pipeline v3 的核心设计假设是“Processor 和 Aggregator 由独占 ProcessThread 串行访问，零锁”。但 HTTP 的 `/collect` 会同时调用 `RunProcessors`，而 ProcessThread 可能正在执行同一批 Processor 的 `Process()`；`Reconfigure` 也会在 `Collect()` 执行时修改 Source 成员。对于有内部状态的 Processor（符号化缓存、过滤规则、聚合器）和 Source（差分快照、`pending_batch_`），这就是**确定的数据竞争**，现有测试没有覆盖到。

**建议**：

引入 **per-feature 命令邮箱（mailbox）**：

```text
HTTP 线程 → FeatureBus::Submit(Command{Start/Stop/Pause/Reconfigure/Query/CollectOnce})
          → Feature 独占的 owner 线程（或 ProcessThread）串行执行
          ← future<Result>
```

- 所有生命周期、配置、按需查询都变成命令，不再从 HTTP 线程触碰数据面对象。
- `RunProcessors` 这类同步采集接口要么通过邮箱返回结果，要么明确标注“仅当 feature 处于 inactive 时可用”。
- 这也是后续实现动态热插拔、回放控制的基础设施。

---

### P0-2 FeatureDriver 生命周期没有串行化，存在 UAF 窗口

**证据**：

- `src/core/engine/feature_driver.h:206-237`：`Probe()` 先读 `state_` 再构建 Pipeline，最后才 `state_.store(kActive)`，状态检查与状态写入不是原子复合操作。
- `src/core/engine/feature_driver.h:240-263`：`Remove()` 同样先读状态，再 `store(kInactive)`，然后取消定时器、停止 Pipeline、`pipeline_.reset()`。
- `src/core/engine/feature_bus.h:78-91`：`FeatureBus::Probe()` 持有的是 **shared_lock**，因此两个 HTTP `start` 请求可以并发进入同一个 driver 的 `Probe()`。
- `src/core/engine/feature_driver.h:388-392`：Flush 定时器回调是 `[this] { pipeline_->InjectFlush(); }`，**没有状态检查**。
- `src/core/engine/feature_driver.h:378-385`：Collect 回调把 `[this]` 提交到 CollectPool，提交后的任务内部只检查 `pipeline_`，没有“Remove 正在发生”的同步。

**具体崩溃路径**：

1. TimerWheel 已弹出 Flush 定时器条目，准备执行回调；
2. HTTP 触发 `Remove()` → `UnregisterTimers()` 只是把 ID 放进 `cancelled_`，无法撤回已弹出的条目；
3. `Remove()` 执行 `pipeline_.reset()`；
4. TimerWheel 回调执行 `pipeline_->InjectFlush()` → **空指针解引用**。

即使没有 Flush 定时器，已提交到 CollectPool 的任务也可能在 `pipeline_.reset()` 后访问 `pipeline_`。`FeatureDriver::GetStats()` / `Info()` / `QueryExtra()` 也会在 API 线程读取 `pipeline_`，与 `Remove()` 的 reset 形成同样的竞争。

**建议**：

1. 每个 Feature 增加 `std::mutex lifecycle_mu_`，所有生命周期转换在其保护下执行（或使用状态机 + CAS）。
2. 增加 `std::atomic<int> in_flight_tasks_`：提交采集任务前 +1，任务结束 -1；`Remove()` 在取消定时器后等待 in_flight 归零，再 `pipeline_.reset()`。
3. Timer 回调不要捕获裸 `this` + 裸 `pipeline_`；改为捕获 `std::weak_ptr<Pipeline>` 或让 `Pipeline` 由 `shared_ptr` 持有（见 P0-3）。
4. `GetStats/Info/QueryExtra/GetPipeline` 返回快照或 `shared_ptr`，不要返回会被 reset 的裸指针。
5. `FeatureBus` 的启停操作改为 unique_lock，或直接走 P0-1 的命令邮箱。

---

### P0-3 Pipeline 的 in-flight 采集任务在 Stop 后仍可访问 Source/Sink

**证据**：

- `src/core/engine/feature_driver.h:378-385`：CollectPool 任务捕获 `this`，内部直接 `pipeline_->GetSource()->Collect()`。
- `src/core/engine/pipeline.h:253-272`：`Enqueue()` 在 `running_` 检查后仍可能被并发 Stop。
- `src/core/engine/pipeline.h:223-249`：`Stop()` 先 `source_->Stop()`，再 join ProcessThread，但没有等待 CollectPool 中已提交的采集任务。

**影响**：动态停止（热拔插）时，Source 可能在 `Stop()` 后、甚至析构后继续被 `Collect()` 调用。对 eBPF Source 意味着访问已 `ring_buffer__free()` 的指针；对 `/proc` Source 意味着访问已释放成员。

**建议**：采用“停止接收 → 等待在途 → 关闭资源”的三段式：

```text
state = Stopping
cancel timers
wait in_flight_collect == 0
pipeline->Stop()
pipeline.reset()
```

同时让 `Pipeline` 的拥有权改为 `shared_ptr`，任务捕获 `weak_ptr`，这样即使任务迟到也只会安全失败。

---

### P0-4 同一个 Sink 会被多个线程并发 Write，导致乱序和数据竞争

**证据**：

- `src/core/engine/pipeline.h:515-527`：`SubmitToSinks` 对每个 Sink、每个 Batch 提交一个独立任务到共享 SinkPool。
- `src/plugin/sinks/sse_sink/sse_sink.h:36-42`：`SseSink::seq_++` 是普通 `uint64_t`，没有锁也没有原子性。
- `src/plugin/sinks/recording_sink/recording_sink.h:62-87`：RecordingSink 内部有 `mutex_`，但只保证不崩溃，不保证写入顺序。

**影响**：

- 对 `SseSink` 是直接的数据竞争（UB），`seq` 可能重复或乱序。
- 时间序列、录制文件、SSE 消息都可能**批次乱序**：Batch 2 先于 Batch 1 被某个 Sink 写入。
- 当前 `SinkPlugin` 接口（`src/plugin/api/sink_plugin.h:32-50`）没有说明线程安全契约。插件作者写一个无锁 Sink 就会踩坑。

**建议**（二选一，推荐第一种）：

1. **Per-sink 有序执行器**：为每个 Sink 维护一条串行队列/单线程执行器，Pipeline 只负责投递；保证同一 Sink 的写入严格按提交顺序。
2. 或者明确在接口文档中要求 Sink 必须线程安全，并为每个 Sink 实例分配独立锁；但这样仍无法保证顺序。

同时把 `SseSink::seq_` 改为 `std::atomic<uint64_t>`（如果仍保留并发写），或改用全局消息序号。

---

### P0-5 录制模块存在裸指针生命周期问题

**证据**：

- `src/plugin/api/recording_interface.h:28-64`：`RecordingSinkRegistry` 存储的是 `RecordableInterface*` 裸指针。
- `src/core/engine/feature_driver.cc:89-110`：`recording_sink_` 也是裸指针，指向 Pipeline 拥有的 Sink。
- `src/server/api_routes.h:304-338`：`/record/status` 先 `Get(name)` 拿到裸指针，再调用 `GetSession()`；与此同时 `/record/stop` 可能正在 `RemoveSinkRuntime()` 销毁该 Sink。

**影响**：两个并发 HTTP 请求（状态查询 + 停止录制）即可触发 **use-after-free**。`RecordingSinkRegistry::ListNames` 只锁住 map，不保护指向对象的生命周期。

**建议**：

- 让 `RecordingSinkRegistry` 持有 `std::shared_ptr<RecordableInterface>`，`Get()` 返回 shared_ptr。
- `FeatureDriver` 也持有 `std::weak_ptr` / `shared_ptr`，不要持有裸指针。
- 录制启停走 P0-1 的命令邮箱，串行化同一 Feature 的控制操作。

---

## 三、P1 — 架构与扩展性问题

### P1-1 插件模型与“只加插件”的目标不匹配

这是最影响长期维护性的问题。

**当前新增一个 Feature 需要改动的文件**：

1. `xxx_source.h`（Source）
2. `xxx_driver.h`（Driver + Describe + ConfigSchema + BuildPipeline）
3. `register.cc`（静态注册）
4. `src/plugin/features/BUILD`（新增 source/driver target）
5. `src/plugin/features/BUILD` 的 `all_sources` / `all_drivers`
6. Driver 内硬编码 `pipeline->AddSink(MakeSseSink(Name()))`
7. `builtin_plugins.cc` 的 include（当前是空文件，依赖 BUILD 的 alwayslink）

**更深层的问题**：

- `src/plugin/infra/plugin_registry.h` 的 `CreateSource/CreateProcessor/...` 在 `FeatureDriver` 的构建路径上**根本没被使用**。`FeatureDriver` 直接 `std::make_unique<CpuUtilizationSource>()`（`cpu_utilization_driver.h:104-116`）。注册表只是“给 CLI/API 看的目录”，不是真正的扩展点。
- 每个 Driver 都重复 `Describe + ConfigSchema + GetConfig + SetConfig + BuildPipeline`，约 130 行样板代码。
- `FeatureDriver` 通过 `static` 全局变量注入 Sse/Recording 工厂（`src/core/engine/feature_driver.cc:6-24`），这是隐式全局依赖，测试和嵌入都难。
- 输出通道（SSE）被硬编码在每个 Driver 里。如果以后要加 WebSocket、OTLP、文件、存储 Sink，需要改所有 Driver。

**建议：引入 `FeaturePack` / 声明式 Feature 描述**

```text
FeaturePack {
  manifest: { name, tier, category, model, config_schema, data_schema, capabilities }
  build(ctx) -> PipelineDraft { source, processors, aggregator }
}
```

- **引擎负责挂 Sink**：根据当前会话配置（SSE / Recording / Storage / Export）统一挂载。Driver 不再知道 SseSink 的存在。
- **一个插件包 = 一个目录 + 一个 BUILD target**，注册由宏/扫描完成，不再维护多个聚合列表。
- 组件通过 `PluginRegistry` 的字符串 ID 解析，或者由 FeaturePack 直接声明工厂；两条路径必须统一，不能像现在这样 Registry 和 Driver 各走各的。

---

### P1-2 `.so` 插件所谓的“稳定 C ABI”实际上不成立

**证据**：

- `src/plugin/api/plugin_api.h:97-119`：`IlPluginDescriptor::create` 返回 `void*`，但 `PluginManager` 把它直接 `static_cast<SourcePlugin*>`（`src/plugin/infra/plugin_manager.h:100-127`）。
- `SourcePlugin` 是带虚函数表的 C++ 类；一个真正的 C 插件无法构造它。
- `destroy` 从未被调用：`unique_ptr` 默认 `delete`，而不是 `desc->destroy(raw)`。
- `process` 函数指针从未使用。
- `create(nullptr)` 不传配置。
- `api_version` 只是一个整数，没有结构体大小、ABI hash、编译器等校验。
- `SoLoader` 只支持加载，没有 `UnloadPlugin`；没有引用计数/epoch，无法安全 `dlclose`。

**影响**：`.so` 插件目前只能由“与主程序完全相同的 C++ 编译器/STL”编译，并且只能注册 Source/Processor/Sink，**无法成为 Feature**。README 中“运行时 `.so` 加载，稳定 C ABI”的表述与实际能力不符。

**建议**：

- 如果坚持 C ABI：用**不透明句柄 + C 函数表**，禁止跨边界传递 `std::string/shared_ptr/vtable`；描述符加入 `struct_size`、`abi_version`、`build_id`；每个组件提供 `create(config_json, errbuf)` / `destroy` / `read/write/flush`。
- 如果接受 C++ SDK：明确“不承诺跨编译器 ABI”，用同一工具链编译，并提供 `sdk/` 头文件与示例插件；不要再宣称稳定 C ABI。
- 增加 **FeaturePack 级描述符**，让 `.so` 能注册一个完整 Feature（source + processors + aggregator + manifest），而不仅是一个组件。
- 增加 **Unload 生命周期**：引用计数 + 在途任务 quiescence + `dlclose`，否则“热插拔”只能做到“热加载”，不能“热卸载”。

---

### P1-3 配置系统有三条路径，且存在“配置不生效”的问题

**证据**：

- `src/core/common/config.h:37-137`：`ConfigValue` 是扁平的 `string -> string` map。
- `src/server/api_v2_routes.h:37-62`：`JsonToConfigValue` 把 JSON 数组拼接成逗号分隔的字符串。
- `src/core/common/config.h:108-128`：`AsList()` 期望 `_size` 索引格式；与上面的逗号拼接**不一致**，数组配置会退化成单元素列表。
- `src/plugin/features/cpu/cpu_utilization/cpu_utilization_driver.h:91-96`：`SetConfig()` 只保存 JSON 字符串，**不调用 Reconfigure**。
- `src/core/engine/feature_driver.h:167-169`：默认 `SetConfig` 直接返回 `kUnimplemented`，但 `Describe()` 默认 `supports_configure = true`。
- `src/core/engine/feature_driver.h:372-373`：定时器间隔在 `Probe()` 时用 `src->IntervalMs()` 固定注册；`CpuUtilizationSource::Reconfigure()`（`cpu_utilization_source.h:119-129`）虽然接受 `interval_ms`，但**定时器不会重新注册**。
- `src/core/engine/timer_wheel.h:128-150`：`AddRepeating` / `AddOnce` 没有校验 `interval > 0`；`interval_ms = 0` 会导致定时器持续以 1ns 触发。

**影响**：

- `/api/v2/features/:name/config` POST 看起来成功，实际不生效。
- `/reconfigure` 修改 interval 后，采集频率不变。
- 数组类型配置无法正确解析。
- 非法值（负数、0、超范围）没有校验，可能造成忙循环或 `uint32_t` 回绕。
- `config_json_` 与实际 Source 配置是两份状态，读回来的配置不代表运行配置。

**建议**：

- 建立**单一配置对象**：Driver 持有一个 typed config，`BuildPipeline`、`Reconfigure`、`GetConfig` 都读写同一份，并持久化。
- 用 JSON Schema 作为唯一声明源，自动生成 `FeatureDescriptor.params`，不要手写两份。
- 增加统一校验层：类型、范围、枚举、正则；非法值返回 `kInvalidArgument`。
- 明确“热更新 / 需重启 / 不支持”三种语义。改变 interval 需要重新注册定时器；不能热更新就返回 `kRequiresRestart`。
- `TimerWheel` 在 API 层加 `interval > 0` 断言/错误返回。

---

### P1-4 数据模型不可扩展，新插件会被迫修改 core

**证据**：

- `src/core/common/data_batch.h:142-252`：`DataBatch` 只有 `records_` 和 `stack_samples_`，`Type` 虽然有 `kTrace/kLog`，但没有对应的结构化载荷。
- `src/core/common/json_serializer.h:100-145`：序列化逻辑按 `Record/StackSample` 硬编码。
- `src/server/storage/storage_backend.h:89-136`：存储接口同样只接受 `Record` 和 `StackSample`。

**影响**：

- 未来要加网络连接事件、日志、调度器事件、任意插件自定义事件，都必须修改 `DataBatch`、JSON 序列化、SQLite 表结构和回放解析器。
- 插件的“数据形状”无法由插件自己声明，core 变成所有数据类型的上帝对象。

**建议**：

引入**通用事件信封 + Schema 注册**：

```cpp
struct DataEnvelope {
  SchemaId schema;        // 插件注册的 schema id
  uint64_t timestamp_ns;
  uint64_t seq;
  std::vector<Label> labels;
  std::vector<Field> fields;   // 或 typed columnar payload
};
```

- 插件在加载时注册 schema（字段名、类型、单位、标签）。
- 序列化、存储、SSE、回放都基于 schema 工作，而不是硬编码 `Record/StackSample`。
- 对时间序列可保留 `Record` 作为一种内置 schema，对堆栈采样保留 `StackSample`。
- 这样“只加插件”才成立：新数据类型不需要改 core。

---

### P1-5 录制/回放不是引擎级能力，难以支撑长期构想

**证据**：

- `src/plugin/sinks/recording_sink/recording_sink.h`：录制是每个 Feature 独立挂载的 Sink，输出 `<feature>_<timestamp>.ilr`。
- `recording_sink.h:80-83`：每个 Batch 都 `flush()`，高频率写入性能差。
- `recording_sink.h:106-107`：用 `std::system("mkdir -p " + output_dir_)`，存在命令拼接风险，且路径含空格会失败。
- `recording_sink.h:67-70`：先 `BatchToJson` 生成字符串，再 `nlohmann::json::parse` 重新解析，重复序列化。
- 后端没有 replay 实现；`docs/frontend_architecture_design.md` 里的 Replay 是前端本地解析。

**影响**：

- 多个 Feature 的录制文件彼此独立，没有会话 manifest、没有统一时间基准、没有配置/内核/版本快照，跨 Feature 对齐困难。
- 录制性能受每批 flush 限制。
- 无法在后端做确定性回放、时间轴控制、快进/变速。

**建议**：把录制/回放提升为**引擎级 Session 子系统**：

```text
Session {
  id, started_at, config_snapshot, kernel/version, feature manifests
  streams: { feature -> stream file }
}

Recorder: 订阅所有 Pipeline 的输出 → 写 manifest + 各 stream（缓冲写）
Replayer: 读取 manifest → 按虚拟时钟驱动 ReplaySource → 复用同一 Pipeline
```

- 统一格式、统一时钟、统一会话。
- `TimerWheel` / `ProcessThread` 的时钟改为可注入的 `IClock`，回放时使用虚拟时钟。
- `.ilr` 从“每 Feature 一个文件”升级为“一个会话目录 + manifest + 流文件”，并加 schema 版本号。
- 存储后端与录制格式共享同一套 schema，避免两套数据格式。

---

### P1-6 eBPF 采集模型没有发挥 ring buffer 的事件驱动优势

**证据**：

- `src/plugin/api/ebpf_source_base.h:245-268`：`Collect()` 在 CollectPool 线程里调用 `ring_buffer__consume()`，并且：

  ```cpp
  auto map_result = CollectFromMaps();
  if (event_batch && !event_batch->Empty()) {
      return event_batch;      // map_result 被直接丢弃
  }
  return map_result;
  ```

- `src/core/engine/infrastructure_manager.h:50-55`：CollectPool 默认只有 2 个线程，所有 Feature 共享。
- `src/core/engine/feature_driver.h:372-385`：按 `IntervalMs()` 定时触发，而不是 ring buffer fd 可读即处理。

**影响**：

- 同时使用 ring buffer 和 BPF map 的 Source 会**丢 map 数据**：只要有事件，map 结果就被丢弃。
- 所有 eBPF Source 共用 2 个采集线程，一个高频 ring buffer 会阻塞其他 Feature 的 `/proc` 采集。
- 事件延迟受采集间隔限制；高频事件在 `ring_buffer__consume` 中一次性处理，可能长时间占用线程。
- 定时器重复触发时没有“单飞”（single-flight）机制：上一次 Collect 未完成，下一次任务仍会被提交，CollectPool 队列会堆积。

**建议**：

- 引入**统一的 eBPF poll 线程**：用一个 epoll 监听所有 ring buffer fd，事件到达即批量读取并投递到 Pipeline；`/proc` 类 Source 继续走 TimerWheel。这正是事件驱动架构应有的形态。
- `Collect` 合并事件和 map 数据，而不是二选一。
- 每个 Source 增加 single-flight 标志，上一次采集未完成时跳过本次触发。
- 为 CollectPool 增加每 Feature 的队列配额或公平调度，避免一个慢 Source 饿死其他 Source。

---

### P1-7 存储层目前在“生产路径”上没有被使用

**证据**：全仓库搜索，`StorageBackend::WriteRecords/WriteStackSamples/WriteProfile` 只在 `src/server/storage/sqlite_backend/test/` 中被调用；生产代码没有任何 Sink 写入 SQLite。`StorageFactory` 只被 CLI 的 `storage` 命令用于列出后端。

**影响**：

- 存储层是“孤岛”：设计完整、测试通过，但与 Pipeline 无连接。
- 录制格式（NDJSON）与存储 schema（`records/stack_samples/profiles` 表）是两套数据模型。
- 无法实现“实时采集 + 离线查询/回放”的统一数据底座。

**建议**：

- 增加 `StorageSink`，或在 Session/Recorder 层统一落盘。
- 统一录制格式与存储 schema，至少共享 schema 定义。
- SQLite 增加 schema version / migration；`Prune` 增加 `override` 并纳入调度（目前没有任何定时 Prune）。
- 明确存储后端的线程模型：当前 `Query/ExecuteRawQuery` 没有和 `write_mutex_` 同步，`Close()` 也没有加锁；SQLite serialized 模式能兜底，但接口上应显式声明。

---

### P1-8 SSE 服务器的线程模型与订阅生命周期

**证据**：

- `src/server/http_server.h:56-60`：cpp-httplib 默认线程池处理连接；`src/server/sse_handler.h:134-223` 的 SSE handler 是**阻塞循环**，会长期占用一个 HTTP 工作线程。
- `sse_handler.h:219-222`：连接关闭时只把 `active` 置 false，**没有从 `subscriptions_` 中删除**。
- 全仓库没有生产代码调用 `Unsubscribe()`（只有测试和定义）。
- `sse_handler.h:320-324, 349-358`：outbox 满时静默丢弃，`messages_dropped` 只计数，没有暴露。
- `sse_handler.h:45-47, 362-369`：每个订阅保留最近 256 条消息，单条最大 64KB，理论 16MB/订阅；分帧消息按帧缓存，实际更高。

**影响**：

- 多个浏览器标签页/客户端建立 SSE 后，HTTP 线程池会被占满，REST 控制面（`/api/v2/features/*`）可能无法响应。
- 断开的订阅永久留在 `subscriptions_` 中，内存和遍历成本持续增长。
- 慢客户端丢数据没有可观测信号，前端无法感知。
- 订阅数无上限，存在本地 DoS 风险。

**建议**：

- SSE 使用独立监听端口或独立的事件循环/线程模型，不要把长连接和控制面 REST 混在同一个 cpp-httplib 线程池里。
- 连接关闭时调用 `Unsubscribe(id)`；增加 `DELETE /api/v1/events/:id`。
- 丢弃事件纳入 `/metrics` 和 `/api/v1/internal_metrics`。
- 为订阅数、队列长度、缓存字节数设置上限。
- 全局消息 ID 只分配一次，所有订阅者复用同一 ID，便于一致的重放和去重。

---

### P1-9 分层与全局单例削弱可测试性和多实例能力

**证据**：

- `src/core/BUILD:76-84`：`core:engine` 依赖 `//src/plugin:api`；而 `plugin:api` 又依赖 `core:common/data/memory`。core 与 plugin 相互依赖。
- `src/core/engine/feature_driver.h:212, 246`：直接调用 `InfrastructureManager::Instance()`。
- `src/core/engine/feature_driver.cc:6-24`：Sse/Recording 工厂放在静态全局变量中。
- `FeatureBus`、`PluginRegistry`、`StorageFactory`、`SseHandler` 都是单例。

**影响**：

- 无法在一个进程中运行多个 Engine 实例（例如同时“实时采集 + 离线回放”）。
- 单元测试必须启动全局 InfrastructureManager。
- 插件作者无法通过依赖注入替换 Timer/Pool/Clock。

**建议**：

- 建立中立的 `sdk/` 层（只含接口、数据类型、插件 API），core 与 plugins 都依赖它，消除 core→plugin 反向依赖。
- 引入 `EngineContext` / `RuntimeContext`：持有 `ITimerService`、`IThreadPool`、`IClock`、`MetricsRegistry`、`PluginResolver`、`SinkFactory`，显式传给 FeatureDriver。
- 单例只保留在 `main` 层做组装，核心类不直接 `Instance()`。

---

### P1-10 CLI 的 `collect` 与 `top` 命令当前实际不可用

**证据**：

- `src/cli/main.cc:200-222`：`RunCollect()` 没有调用 `SetSseSinkFactory` / `SetRecordingSinkFactory`。
- `src/plugin/features/cpu/cpu_utilization/cpu_utilization_driver.h:119`：`AddSink(MakeSseSink(Name()))`，工厂为空时返回 `nullptr`。
- `src/core/engine/pipeline.h:171-174`：Pipeline 没有 Sink 时 `Start()` 返回 `kInvalidArgument`，`ProbeAll()` 只打 WARN。
- `src/cli/main.cc:232-237`：`RunTop()` 使用 `CreateSource("proc_stat_reader")`，但该 Source 已不存在，启动后直接报错。

**建议**：

- 抽一个 `Bootstrap(config)`：统一设置所有工厂、注册驱动、初始化 Infrastructure；daemon/collect/top 共用。
- `top` 直接使用 `CpuUtilizationSource` 或走 FeatureBus，而不是查找已删除的插件名。
- 增加 CLI 冒烟测试（`collect --duration 1`），否则这类问题会持续回归。

---

## 四、P2 — 性能与可维护性优化

| 编号 | 问题 | 证据 | 建议 |
|------|------|------|------|
| C1 | 每个 DataBatch 创建一个 64KB Arena | `data_batch.h:154-155`，`arena.h:47-53` | 引入 BatchPool/ArenaPool，复用批次对象和 Arena |
| C2 | 每条 Record 使用 `unordered_map<string_view, FieldValue>` | `data_batch.h:83-98` | 高频指标改为 columnar/schema 化布局；至少减少节点分配 |
| C3 | InternalMetrics 全局 mutex，且在 Sink 热路径上 SetGauge | `self_observability.h:65-86`，`pipeline.h:497-505` | 改为 per-pipeline 原子计数器/分片指标；热路径只做原子累加 |
| C4 | Histogram 只有 min/max/avg，没有 bucket | `self_observability.h:79-86` | 引入固定 bucket 的直方图，支持 P50/P99 |
| C5 | ThreadPool 无界队列，`Submit` 使用 `packaged_task + bind + future` | `thread_pool.h:76-93` | 增加有界队列和拒绝策略；无返回值的任务走轻量 `std::function` 路径 |
| C6 | `DrainAndStop` 后 `Submit` 仍会入队，永不执行 | `thread_pool.h:76-93, 96-105` | `Submit` 检查 `stop_`，返回错误/丢弃 |
| C7 | SinkPool 全局共享，PendingTasks 锁竞争，且每批提交 N 个任务 | `pipeline.h:493-527` | 每 Sink 有序队列 + 批量提交；按 Pipeline 预算隔离 |
| C8 | 丢弃/错误日志在热路径无条件打印 | `pipeline.h:258, 485, 508, 521` | 速率限制/采样日志，或仅在状态变化时打印 |
| C9 | PidManager O(n²) 去重 + 全量 BPF map 重写；Timer 回调捕获裸 `this` | `pid_manager.h:77-91, 136-150, 198-208` | 用 `unordered_set`；回调改为 weak 引用；增量同步 BPF map |
| C10 | 进程 RSS 假设页大小 4KB | `process_cpu_source.h:161` | 使用 `sysconf(_SC_PAGESIZE)` |
| C11 | 静态注册宏放在头文件中，每个包含 TU 都会注册一次 | `cpu_utilization_source.h:276`，`sqlite_backend.h:609-614` | 注册移到 `.cc`，头文件只声明 |
| C12 | Driver 样板代码 + JSON Schema 与 Describe 重复 | `cpu_utilization_driver.h:45-125`，`process_cpu_driver.h:38-104` | 用 FeaturePack/Schema 派生，减少重复 |
| C13 | v1/v2 API 并存，语义重叠 | `api_routes.h:169-203`，`api_v2_routes.h` | 冻结 v1 为兼容别名，新功能只进 v2；给出弃用时间表 |
| C14 | SQLite 无 schema migration；`Query.total_count` 只是返回行数；`Close` 未加锁 | `sqlite_backend.h:212-337, 381-387` | 加 `user_version`/migration；明确 total/has_more；生命周期加锁 |
| C15 | 构建依赖系统 `libbpf/sqlite3/bpftool/clang`，Bazel 非完全 hermetic | `bpf_probe.bzl`，`.bazelrc` action_env | 长期考虑 toolchain/外部仓库固定版本 |
| C16 | 测试缺少并发生命周期、ABI、回放确定性用例 | `src/core/engine/test/*`，`src/plugin/test/*` | 增加 TSAN 场景：并发 Probe/Remove/Reconfigure/HTTP collect；`.so` 加载/卸载；回放重复性 |

---

## 五、目标架构建议

结合长期构想（动态热插拔、实时采集、实时/离线可视化、离线回放），建议把目标架构定为下面这样：

```text
┌──────────────────────────────────────────────────────────────┐
│  SDK Layer（稳定接口 + 数据类型 + 插件 ABI）                   │
│  DataEnvelope / Schema / ErrorCode / FeaturePack / PluginApi   │
└───────────────▲──────────────────────────▲───────────────────┘
                │                          │
┌───────────────┴──────────────┐  ┌────────┴──────────────────┐
│ Core Runtime                 │  │ Plugin Packs              │
│  EngineContext               │  │  builtin packs            │
│  FeatureBus + Feature        │  │  .so packs                │
│  Pipeline / Scheduler        │  │  manifest + components    │
│  Session / Recorder / Replay │  └───────────────────────────┘
│  Clock / Metrics / Storage   │
└───────────────▲──────────────┘
                │ commands (mailbox)
┌───────────────┴──────────────┐
│ Server（控制面）              │
│  REST / SSE / Replay API     │
└──────────────────────────────┘
```

### 关键设计原则

1. **控制面只发命令，不碰数据面**
   - 每个 Feature 一个命令队列和独占执行线程。
   - Start/Stop/Pause/Reconfigure/Query/CollectOnce 全部串行化。
   - HTTP handler 只做参数解析 + `Submit` + 等待 future。

2. **生命周期三段式**
   - `Running → Stopping → Inactive`。
   - 取消定时器 → 等待在途任务 → 关闭 Source → 停止 Pipeline → 释放。
   - 所有异步任务持 `weak_ptr`，停止后安全失败。

3. **FeaturePack 作为插件单元**
   - 插件声明 metadata、config schema、data schema、source/processor/aggregator 组装方式。
   - 引擎根据 Session 配置统一挂 Sink。
   - 插件作者不需要知道 SSE、录制、存储的存在。

4. **Sink 串行化 + 数据 schema 化**
   - 每个 Sink 实例一条有序队列，保证写入顺序。
   - 数据使用通用 `DataEnvelope` + 插件注册的 schema。
   - 序列化、存储、SSE、回放共用同一 schema。

5. **Session 统一录制/回放**
   - 一个 Session = manifest + 多路 stream + 配置/内核/版本快照。
   - 回放使用虚拟时钟，复用真实 Pipeline。
   - 录制的格式与存储格式统一，避免两套数据模型。

6. **EngineContext 替代全局单例**
   - Timer/Pool/Clock/Metrics/SinkFactory 显式注入。
   - 支持一个进程内多 Engine（实时 + 回放）。
   - 大幅提升可测试性。

7. **插件 ABI 明确承诺**
   - C ABI 就必须严格 C：opaque handle、C 函数表、`struct_size`、`destroy`、错误码、配置 JSON。
   - 否则就明确 C++ SDK + 同工具链，不要混称“稳定 C ABI”。
   - 支持 Unload：引用计数 + quiescence + `dlclose`。

---

## 六、建议的落地顺序

### 第 0 阶段：先止血（约 1 周）

1. FeatureDriver 加生命周期互斥 + in-flight 计数；Flush 回调加状态/weak 检查。
2. HTTP `/collect`、`/reconfigure`、`/query` 改走命令队列或明确限制。
3. Sink 按实例串行化；`SseSink::seq_` 原子化；明确 Sink 线程安全契约。
4. Recording 注册表/句柄改 `shared_ptr`；去掉 `system("mkdir")`。
5. Timer interval 校验；Reconfigure 不支持的参数返回 `kRequiresRestart`。
6. 修复 `collect`/`top` 的启动路径，加 CLI 冒烟测试。

### 第 1 阶段：内核升级（约 2–4 周）

7. 引入 `EngineContext`，消除 core 对 plugin 的反向依赖和全局工厂。
8. 引入 `FeaturePack` + 引擎统一挂 Sink，消灭 Driver 里的 `MakeSseSink` 硬编码。
9. 统一配置对象 + JSON Schema 校验 + 持久化。
10. 引入 `DataEnvelope` / Schema 注册，改造序列化与存储接口。
11. 引入 Session/Recorder/Replayer + 可注入时钟。
12. eBPF 统一 poll 线程 + single-flight + 事件/map 合并。

### 第 2 阶段：生态与硬化（持续）

13. 插件 ABI v2 + `.so` FeaturePack + Unload + SDK/示例。
14. SSE 独立数据面 + 订阅清理 + 背压指标。
15. Metrics 分片/直方图 + ResourceLimiter 真正执行限流。
16. TSan 并发测试、回放确定性测试、配置 fuzz、`.so` 兼容性测试。

---

## 七、附录：问题索引

| 严重度 | 问题 | 位置 |
|--------|------|------|
| P0 | HTTP 直接 Collect / RunProcessors / Reconfigure | `src/server/api_routes.h:189,194`；`feature_driver.h:300-305`；`pipeline.h:330-360` |
| P0 | Feature 生命周期无串行化 | `feature_driver.h:206-263`；`feature_bus.h:78-91` |
| P0 | Flush 定时器裸指针 UAF | `feature_driver.h:388-392` |
| P0 | 在途 Collect 任务在 Remove 后访问 Pipeline | `feature_driver.h:378-385`；`pipeline.h:223-249` |
| P0 | Sink 并发写与乱序 | `pipeline.h:515-527`；`sse_sink.h:36-42` |
| P0 | Recording 裸指针 UAF | `recording_interface.h:28-64`；`feature_driver.cc:89-110` |
| P1 | 插件不是 Feature 单元，Driver 硬编码 SseSink | `plugin_registry.h:80-190`；`cpu_utilization_driver.h:104-122` |
| P1 | C ABI 名不副实 | `plugin_api.h:97-119`；`plugin_manager.h:93-131` |
| P1 | 配置不生效/无校验/数组解析不一致 | `config.h:108-128`；`api_v2_routes.h:37-62`；`cpu_utilization_driver.h:91-96` |
| P1 | 数据模型不可扩展 | `data_batch.h:142-252`；`json_serializer.h:100-145` |
| P1 | 录制/回放非引擎级 | `recording_sink.h`；无后端 replay |
| P1 | eBPF map 数据被丢弃 / 无统一 poll | `ebpf_source_base.h:245-268`；`infrastructure_manager.h:50-55` |
| P1 | 存储层未接入生产 | `storage_backend.h`；仅测试调用 |
| P1 | SSE 线程池占用 + 订阅泄漏 + 静默丢弃 | `sse_handler.h:134-223,312-369` |
| P1 | core ↔ plugin 反向依赖 + 全局单例 | `src/core/BUILD:76-84`；`feature_driver.cc:6-24` |
| P1 | collect/top 命令不可用 | `main.cc:200-222,232-237` |
| P2 | Arena/Batch 分配、Metrics 全局锁、ThreadPool、日志热路径等 | 见第四节 |

---

## 结论

**Illuminator 现在的“引擎设计”已经超过大多数同类项目的初期水平，尤其是 Pipeline v3、反压、eBPF 基类和 FeatureDriver 抽象。**

但从“能跑”到“能长期只靠加插件演进”，还差一层**运行时边界**：控制面与数据面必须通过命令通道隔离，Feature 生命周期必须可串行、可等待、可回滚，插件单元必须是 FeaturePack 而不是散落的 Source/Sink 类，数据与录制必须 schema 化、会话化。

建议先完成第 0 阶段的 6 个止血项，再启动第 1 阶段的 EngineContext + FeaturePack + Session 重构。越过这些直接加插件，后续每加一个 Feature，都会把当前的竞态和耦合再复制一份。
