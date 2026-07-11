# RFC: eBPF Push Mode 高性能热插拔

- **状态**: Draft
- **作者**: illuminator-team
- **日期**: 2026-07-11
- **关联**: perf_ebpf CtrlPlane 对比分析

## 问题

当前 `FeatureDriver::Pause()` 仅调用 `UnregisterTimers()`，对 Push 模式的 eBPF 源
（`EbpfRingBufferSource` 及其子类）**完全无效**：

1. Push 源从不注册 collect timer — 没有定时器可取消
2. ringbuf poll 线程持续运行（`running_` 标志不受 Pause 影响）
3. BPF 程序仍在内核侧发射事件到 ringbuf
4. callback 持续推送数据到 Pipeline

这意味着 REST API `POST /api/v2/features/:name/pause` 对所有 push-mode feature
（io_monitor、net_tracer、sched_tracer 等）是一个 **silent no-op bug**。

## 目标

实现纳秒级的 Push 源暂停/恢复，满足：

1. **零内核开销暂停**: 暂停后 BPF 程序不产生事件，不消耗 ringbuf 空间
2. **无数据丢失恢复**: 恢复时不丢失过渡期事件
3. **统一接口**: Pull/Push 源共用相同的 `Pause()`/`Resume()` API
4. **向后兼容**: 不修改现有 Pull 源的行为

## 方案选型

### 对比分析

| 方案 | 暂停开销 | 恢复开销 | 数据丢失 | 内核开销 |
|------|---------|---------|---------|---------|
| A: BPF Map Gate | 1 次 map 更新 (~ns) | 1 次 map 更新 (~ns) | 无 | ~5ns/触发 |
| B: bpf_link detach/reattach | detach (~μs) | reattach (~ms) | 有 | 0 |
| C: 用户态 drain | 设标志 (~ns) | 设标志 (~ns) | ringbuf 溢出 | 全量 |
| D: rodata reload | load (~ms) | load (~ms) | 有 | 0 |

### 选定：方案 A — BPF Map Gate

**理由**：
- 在 10 万次/秒 tracepoint 触发率下，每次 gate 检查 ~5ns，总开销 0.05% CPU
- perf_ebpf 的 `CtrlPlane` + `ctrl_map` 验证了同类方案的可行性
- illuminator 已有类似模式（`cpu_profiler_cfg`、`offcpu_cfg` bit4）

## 设计

### 1. BPF 侧：统一 Gate Map 约定

在 `common.bpf.h` 中定义标准 gate 基础设施：

```c
// ---- Collection Gate（Push 源暂停/恢复控制）----
// 所有 push-mode BPF 程序必须声明此 map 并在入口调用 CHECK_GATE()

#define DECLARE_COLLECTION_GATE() \
    struct { \
        __uint(type, BPF_MAP_TYPE_ARRAY); \
        __uint(max_entries, 1); \
        __type(key, __u32); \
        __type(value, __u32); \
    } collection_gate SEC(".maps")

#define CHECK_GATE() do { \
    __u32 _gate_key = 0; \
    __u32 *_gate_val = bpf_map_lookup_elem(&collection_gate, &_gate_key); \
    if (!_gate_val || !*_gate_val) return 0; \
} while(0)
```

每个 push-mode BPF 程序的 tracepoint handler 入口添加 `CHECK_GATE()`。

**已有 gate 的程序**（复用现有 cfg map）：
- `cpu_profiler.bpf.c` → `cpu_profiler_cfg` bit0
- `offcpu_profiler.bpf.c` → `offcpu_cfg` bit4
- `sched_analyzer.bpf.c` → `sched_analyzer_cfg`

**需要新增 gate 的程序**：
- `bio_latency.bpf.c` → 新增 `collection_gate`
- `net_tracer.bpf.c` → 新增 `collection_gate`
- `sched_tracer.bpf.c` → 新增 `collection_gate`

### 2. 用户态 Source API 扩展

`SourcePlugin` 新增两个虚方法：

```cpp
virtual Status PauseCollection() { return Status::Ok(); }
virtual Status ResumeCollection() { return Status::Ok(); }
```

Pull 源默认空操作（由 FeatureDriver 停定时器即可）。
Push 源在子类中覆写，实现 BPF gate 控制 + poll 线程管理。

### 3. EbpfRingBufferSource 基类增强

提供 gate-based 的暂停/恢复默认实现：

- `PauseCollection()`: 关闭 BPF gate → 停止 poll 线程
- `ResumeCollection()`: 启动 poll 线程 → 打开 BPF gate
- 子类只需覆写 `GateMapFd()` 返回 gate map 的 fd

### 4. FeatureDriver 改造

`Pause()` 和 `Resume()` 增加 push-mode 分支：

```
Pause:
  1. UnregisterTimers()          ← Pull 模式需要
  2. source->PauseCollection()   ← Push 模式需要

Resume:
  1. source->ResumeCollection()  ← Push 模式需要
  2. RegisterTimers()            ← Pull 模式需要
```

### 5. 时序保证

```
Pause 序列（先关门后停车）:
  T1: bpf_map_update_elem(gate, 0)   → 内核停止发射事件
  T2: drain ringbuf 残留事件          → 消费完成
  T3: join poll_thread                → 安全退出

Resume 序列（先发车后开门）:
  T1: 启动 poll_thread               → 消费端就绪
  T2: bpf_map_update_elem(gate, 1)   → 内核开始发射事件
```

## 影响范围

### 新增文件
- 无

### 修改文件

| 文件 | 改动 |
|------|------|
| `src/ebpf/include/common.bpf.h` | 新增 `DECLARE_COLLECTION_GATE` 和 `CHECK_GATE` 宏 |
| `src/ebpf/probes/io/bio_latency.bpf.c` | 新增 gate map + CHECK_GATE |
| `src/ebpf/probes/net/net_tracer.bpf.c` | 新增 gate map + CHECK_GATE |
| `src/ebpf/probes/sched/sched_tracer.bpf.c` | 新增 gate map + CHECK_GATE |
| `src/plugin/api/source_plugin.h` | 新增 PauseCollection/ResumeCollection |
| `src/plugin/sources/ebpf_ring_buffer_source.h` | 实现 gate-based pause/resume |
| `src/core/engine/feature_driver.h` | Pause/Resume 增加 push-mode 分支 |
| `src/plugin/sources/cpu/cpu_profiler/cpu_profiler.h` | 覆写 PauseCollection（复用 cfg） |
| `src/plugin/sources/sched/sched_analyzer/sched_analyzer.h` | 覆写 PauseCollection（复用 cfg） |
| `src/plugin/sources/sched/offcpu_profiler/offcpu_profiler.h` | 覆写 PauseCollection（复用 cfg bit4） |

### 性能影响
- 每次 tracepoint 触发增加 ~5ns 的 gate 检查（ARRAY map lookup）
- 在 10 万次/秒频率下额外开销 ≈ 0.05% CPU
- 暂停/恢复切换延迟 < 1μs

### 向后兼容性
- Pull 源行为不变
- REST API `/pause` `/resume` 接口不变
- BPF gate 默认值为 1（active），启动行为不变

## 与 perf_ebpf CtrlPlane 的关系

本方案借鉴了 perf_ebpf 的 `CtrlPlane` + `ctrl_map` 设计思路：
- 统一控制 map 类型（ARRAY）
- 用户态广播配置到 BPF 程序
- 内核侧在热路径上检查控制标志

区别在于 illuminator 的实现更轻量（单一 gate 标志），而 perf_ebpf 的 CtrlPlane
支持更复杂的模式切换（mode/flags/target_pids）。未来可以扩展为完整的控制平面。

## 测试计划

1. 单元测试：验证 Push 源的 Pause/Resume 状态转换
2. 集成测试：启动 eBPF feature → pause → 验证无事件输出 → resume → 验证恢复
3. 压力测试：高频 sched_switch 下的 gate 检查开销测量
4. 兼容性测试：Pull 源的 Pause/Resume 行为不变
