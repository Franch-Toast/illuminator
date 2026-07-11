# perf_ebpf vs illuminator 架构对比分析

> **更新日期**: 2026-06-16（代码审计修正版）

## 1. 项目定位对比

| 维度 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **定位** | 车端生产级性能观测守护进程 | 通用高性能可观测平台（含 Web UI） |
| **目标环境** | QNX/Linux 嵌入式（多车型） | Linux 服务器/容器 |
| **BPF 程序数** | **17 个**（含 DWARF 展开器） | **8 个**（7 已编译集成 + 1 孤立） |
| **用户态插件数** | 14 个 Plugin | 9 Source + 4 Processor + 9 Sink |
| **构建系统** | Bazel + `bpf_rules.bzl` | Bazel + `bpf_probe.bzl` |
| **数据输出** | 文件(zstd) + Church 事件上报 | SSE 实时推送 + HTTP REST + SQLite |
| **配置格式** | JSON（v1/v2，支持 `extends` 继承） | YAML（单文件 + 内置默认） |
| **前端** | 无（由 FlameCraft 等离线分析） | 内置 React SPA 实时可视化 |

## 2. 架构差异

### 2.1 数据流

```
perf_ebpf:
  BPF → perf_event_array/ringbuf → 插件线程直接处理 → Sink(文件)
  特点：路径短，无中间队列

illuminator:
  BPF → ringbuf → Source → AsyncChannel → Processor → SinkPool → SSE/SQLite
  特点：灵活组合，但链路长，背压传递慢
```

### 2.2 插件体系

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **注册** | `REGISTER_PLUGIN` 静态构造 | `IL_REGISTER_SOURCE/PROCESSOR/SINK` |
| **生命周期** | init → start → stop → rotate | Init → Start → Stop |
| **线程模型** | 每插件独立轮询线程 | InfrastructureManager 线程池 + per-Pipeline ProcessThread |
| **探针管理** | 插件自管理 skeleton | 插件自管理 skeleton（EbpfSkeletonSource 基类 + 自定义 SourcePlugin） |

### 2.3 BPF Buffer 抽象

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **主路径** | `perf_event_array`（compat 层封装） | `BPF_MAP_TYPE_RINGBUF` |
| **满时行为** | 内核丢弃，`handle_lost_events` 统计 | `bpf_ringbuf_reserve` 返回 NULL |
| **对调度器影响** | 丢弃不阻塞 BPF 程序 | reserve 失败后 BPF 直接返回 |
| **heap 机制** | per-CPU heap 10KB 避免栈溢出 | 无 |

## 3. off-CPU 安全防护对比（核心差异）

### 3.1 过滤顺序（最关键差异）

```
perf_ebpf（安全）:
  sched_switch → is_target_process_offcpu(pid)?
                 └── NO → return（≤16 PID 线性扫描）
                 └── YES → is_kernel_thread?
                            └── YES → user_stack=-1
                            └── NO → bpf_get_stackid(USER_STACK)

illuminator（修复后）:
  sched_switch → PID 白名单?
                 └── NO → skip phase1
                 └── YES → comm 白名单?
                            └── NO → skip phase1
                            └── YES → kthread?
                                       └── YES → user_stack=-1
                                       └── NO → bpf_get_stackid(USER_STACK)
```

### 3.2 事件模型差异

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **off-CPU 事件传递** | 内核 map 聚合 → 1Hz 信号 → 用户态批量读 | 每事件 ringbuf_reserve → submit |
| **聚合位置** | BPF `offcpu_time_stats_extended` map | 无（每事件独立推送） |
| **用户态触发** | `OFFCPU_AGGREGATION_EVENT` 信号（≤1Hz） | ring_buffer__poll 持续轮询 |
| **热路径开销** | 仅 map 更新（极低） | ringbuf reserve+submit（中等） |

**perf_ebpf 聚合模型的优势**：
- sched_switch 热路径仅做 map 更新，无内存分配
- 用户态最多 1 秒收到一次通知，不会被事件洪水淹没
- 相同 stack 的事件自动合并（按 stack key 聚合计数+时长）

### 3.3 阈值管理

| 方面 | perf_ebpf | illuminator（修复前） |
|------|-----------|---------------------|
| **默认阈值** | 编译期 100ms，生产配置 **1ms** | 编译期 10ms |
| **配置方式** | skeleton rodata 写入（open 后 load 前） | `const volatile` 编译期固定 ❌ |
| **运行时可调** | ✅ 配置文件 → rodata → BPF 程序 | ❌ 配置值仅 C++ 端，BPF 不可见 |

### 3.4 启动延迟

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **延迟策略** | PID map 启动为空 → 等效关闭 off-CPU 5s | 管道间 100ms 延迟 |
| **PID 发现** | `/proc` 扫描 + 进程名匹配 → 写入 BPF map | 配置文件静态指定 |
| **动态刷新** | 每 30s 重扫 `/proc` | 无（启动时一次性写入） |
| **延迟目的** | 等待系统稳定后再采集 | 避免多探针同时挂载 |

## 4. illuminator 应借鉴的关键设计

### P0（防系统冻结，必须实现）

| 项目 | 说明 | perf_ebpf 参考 |
|------|------|----------------|
| **min_duration_ns 运行时可配** | 当前 BPF rodata 是编译时常量，用户配置无效 | `skel->rodata->offcpu_min_duration_ns = config->...` |
| **PID map 空 = 全禁** | 启动时/配置为空时，BPF 层完全跳过 off-CPU 栈展开 | `is_target_process_offcpu` count=0 → false |
| **聚合信号代替每事件推送** | 内核聚合 + 低频通知 → 极大降低 sched 热路径开销 | `offcpu_time_stats_extended` + 1Hz signal |

### P1（稳定性增强，强烈推荐）

| 项目 | 说明 | perf_ebpf 参考 |
|------|------|----------------|
| **PID 动态刷新** | 定期扫描 /proc 更新 target_pids | `pid_manager_thread` 30s 周期 |
| **Buffer 满统计** | ringbuf reserve 失败时计数 → 可观测 | `meta_stats_map` META_STAT_BUFFER_FULL |
| **D-state 独立阈值** | 长期阻塞（D-state）可用更高阈值 dump 栈 | `dump_stack_threshold_ns` 100ms |
| **perf_event_array fallback** | 对溢出更友好的 buffer 类型 | `compat.cc` 统一抽象 |

### P2（架构改进，中期规划）

| 项目 | 说明 | perf_ebpf 参考 |
|------|------|----------------|
| **DWARF 展开支持** | 无帧指针二进制的深栈解析 | `native_unwinder.bpf.c` + `eh_frame_parser` |
| **v2 配置继承** | 多环境配置复用 | `extends` + `output` 注入 |
| **Session 管理** | 数据轮转 + 元数据 | `SessionManager` |
| **BPF skeleton** | 类型安全的 BPF 交互 | 全面使用 libbpf skeleton |

## 5. BPF 编译与加载对比（关键架构差异）

### 5.1 编译流程

| 阶段 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **BPF 源码** | `bpf_common/*.bpf.c` | `src/ebpf/probes/**/*.bpf.c` |
| **Bazel 规则** | `bpf_rules.bzl → bpf_program()` | `bpf_probe.bzl → bpf_probe()` |
| **编译器** | `clang-14` 固定 | `$BPF_CLANG`（默认 clang） |
| **编译参数** | `-g -O2 -target bpf -D__TARGET_ARCH_x86` | `-g -O2 -target bpf -D__TARGET_ARCH_x86` |
| **vmlinux** | 预生成 x86_64 + arm64 | 仅 x86_64（系统头） |
| **Skeleton 生成** | ✅ `bpftool gen skeleton` → `*.skel.h` | ✅ `bpf_skeleton()` → `*.skel.h`（已迁移） |
| **Object 打包** | ✅ `bpftool gen object` (CO-RE reloc) | 直接编译 `.bpf.o` → skeleton 嵌入 |

### 5.2 运行时加载

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **加载方式** | Skeleton API (`*_bpf__open/load/attach`) | ✅ Skeleton API（已迁移，`*_sk_bpf__open/load/attach`） |
| **Map 访问** | `skel->maps.xxx` 类型安全 | ✅ `skel_->maps.xxx` 类型安全（已迁移） |
| **Prog 访问** | `skel->progs.xxx` 类型安全 | ✅ `skel_->progs.xxx` 类型安全（已迁移） |
| **rodata 配置** | ✅ `skel->rodata->param = value` | ❌ 未使用（配置通过 BPF map 注入，可热更新） |
| **Map 复用** | ✅ `bpf_map__reuse_fd()` 跨 BPF | ❌ 无（待引入） |
| **类型安全** | 强（编译期检查） | ✅ 强（skeleton 编译期检查，已迁移） |
| **RAII** | `*_bpf__destroy()` | ✅ `*_sk_bpf__destroy()` skeleton 析构 |

### 5.3 关键差异影响

**perf_ebpf 使用 skeleton 的优势：**
1. BPF rodata 可在加载前由 C++ 设置 → 配置真正传入 BPF 程序
2. 编译期验证 map 名称和结构体布局 → 不存在字符串拼写风险
3. `bpf_map__reuse_fd` 支持跨 BPF 程序共享 map（如 sched ↔ native_unwinder）

**illuminator 已完成 skeleton 迁移后的剩余差距：**
- rodata 配置注入尚未使用（配置通过 BPF map 在运行时传递，可热更新但有 ~50ns map 查找开销）
- Map 跨 BPF 复用（`bpf_map__reuse_fd`）尚未引入

### 5.4 编译选项

| 选项 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **C++ 标准** | C++17 | C++20 |
| **优化** | 框架 -O2，主程序 -O0 | -O2 |
| **帧指针** | 不强制（有 DWARF 展开） | **必须** `-fno-omit-frame-pointer` |
| **BPF 编译** | `clang-14 -g -O2 -target bpf` | `clang -g -O2 -target bpf` |

**关键约束**：illuminator 仅支持 FP-based 栈展开（`bpf_get_stackid(BPF_F_USER_STACK)`），
目标二进制**必须**带帧指针。perf_ebpf 的 `native_unwinder` 支持 DWARF 展开，无此限制。

## 6. 采集频率风险评估（是否会导致系统卡死）

### 6.1 illuminator 各组件采集频率清单

| 组件 | 频率 | 触发方式 | 风险等级 |
|------|------|---------|---------|
| **On-CPU profiler** | **49 Hz** per CPU | perf_event 软中断 | 🟢 安全（perf 标准值） |
| **Off-CPU profiler** | 事件驱动 + **≤1 Hz** 信号 | sched_switch tracepoint | 🟢 安全（map 聚合） |
| **CPU utilization** | 1000 ms | TimerWheel pull | 🟢 安全 |
| **CPU processes** | 2000 ms | TimerWheel pull | 🟢 安全 |
| **Sched analyzer** | 5000 ms | TimerWheel pull | 🟢 安全 |
| **IO monitor** | 事件驱动 | tracepoint | 🟡 取决于 I/O 负载 |
| **Net tracer** | 事件驱动 | kprobe | 🟡 取决于网络负载 |
| **Ringbuf poll** | 100 ms timeout | poll 系统调用 | 🟢 安全 |
| **SSE push** | 事件驱动 | SseSink → SseHandler 条件变量 | 🟢 安全 |
| **TimerWheel** | 1000 ms epoll | 内部调度 | 🟢 安全 |
| **Storage prune** | 60000 ms | 定时器 | 🟢 安全 |

### 6.2 结论：**正常情况不会卡死**

**安全的设计选择：**
- 49 Hz 采样率远低于 perf_ebpf v2 的 299 Hz，属于非常保守的值
- Off-CPU 使用 map 聚合 + 1Hz 信号（已学习 perf_ebpf），热路径开销极低
- procfs 读取间隔 1-2s，不会产生 proc_stat 竞争

**潜在风险场景（极端情况）：**

| 场景 | 可能后果 | 当前防护 | 建议 |
|------|---------|---------|------|
| 全部 5 个 feature 同时启动 | 5 条 BPF 探针 + 5 个 poll 线程 | 100ms 挂载错开 | 🟢 已有 |
| IO/Net 事件风暴（万级 IOPS） | ringbuf 满 → reserve 返回 NULL（事件丢失） | BPF 直接返回 | 🟡 无丢失统计 |
| 目标进程子线程数 > 1000 | 每次 collect 解析大量 /proc/<pid>/task/ | 无上限 | 🟡 应添加采样 |
| 长时间运行（>1h）无停止 | RSS 缓慢增长（SQLite + in-memory buffers） | ResourceLimiter 512MB 报警 | ⚠️ 仅报警不强制 |
| 内核 perf_event_paranoid ≥ 2 | 非 root 无法采样 → 静默无数据 | 启动日志提示 | 🟢 不会卡 |

### 6.3 perf_ebpf vs illuminator 安全模型对比

| 防护层 | perf_ebpf | illuminator | 差距 |
|--------|-----------|-------------|------|
| **PID 白名单空 = 全禁** | ✅ 强制 | ✅ Tier3 需指定 PID | 🟢 等效 |
| **启动延迟** | 5s PID map 为空 | 3s `start_delay_seconds` | 🟢 接近 |
| **内核 map 聚合** | ✅ off-CPU | ✅ off-CPU | 🟢 已学习 |
| **内核线程跳过** | ✅ `PF_KTHREAD` | ✅ | 🟢 |
| **采样率背压降级** | 未实现 | ✅ 49→12 Hz | 🟢 illuminator 更好 |
| **Buffer 满丢失计数** | ✅ `meta_stats_map` | ❌ 无统计 | 🔴 差距 |
| **CPU 开销自动禁用** | ❌ 未实现 | ❌ 仅 warn | 🟡 双方都缺 |
| **磁盘限额强制** | ✅ `max_total_size_gb` | 🟡 SQLite prune 30min | 🟡 |
| **动态 PID 刷新** | ✅ 30s 周期 | ❌ 一次性写入 | 🔴 差距 |

### 6.4 安全机制总结

perf_ebpf **不冻结系统**的根本原因（按重要性排序）：

1. **off-CPU 仅对 ≤16 个目标 TGID 做用户栈展开** — 非目标进程的 sched_switch 开销极低
2. **启动延迟期 PID map 为空** — 等价于 off-CPU 完全关闭，系统先稳定
3. **内核 map 聚合 + 1Hz 信号** — sched 热路径不做 ringbuf 分配
4. **perf_event_array 溢出丢弃** — 不阻塞 BPF 执行
5. **1ms 阈值（非 100μs）** — 过滤掉大量正常调度切换
6. **内核线程跳过** — kworker 等高频切换的线程不做 USER_STACK

**illuminator 不冻结系统的保障：**

1. **49 Hz 采样（保守值）** — 每 CPU 每秒仅 49 次中断
2. **off-CPU map 聚合 + 1Hz** — 已复制 perf_ebpf 模型
3. **Tier 3 PID 强制** — Profiling 类探针必须指定目标
4. **背压 → 降频** — AsyncChannel 满 80% 时自动降到 12 Hz
5. **BPF ringbuf reserve 失败 = 静默丢弃** — 不阻塞 BPF 热路径

## 7. illuminator 已学习并实现的 perf_ebpf 机制

| perf_ebpf 机制 | illuminator 实现状态 | 备注 |
|---------------|--------------------|----|
| TGID-based PID 过滤 | ✅ `bpf_get_current_pid_tgid() >> 32` | 自动覆盖进程所有线程 |
| `group_leader->comm` 过滤 | ✅ `BPF_CORE_READ(task, group_leader, comm)` | 子线程改名不影响 |
| tgid+tid 分离的聚合 key | ✅ `offcpu_stat_key{tgid, tid, ...}` | 精确区分多线程 |
| BPF map 聚合 + 1Hz 信号 | ✅ `offcpu_stats` + ringbuf | 低频用户态通知 |
| 启动延迟 + 全局使能 | ✅ `start_delay_seconds` + bit4 flag | 系统稳定后才开始 |
| 内核线程跳过 (PF_KTHREAD) | ✅ 检查 `task->flags` | 避免无效展开 |
| PID namespace 处理 | ✅ `/proc/self/maps` 回退 | 容器场景兼容 |
| ELF load_base 归一化 | ✅ 解析 PT_LOAD 段 | PIE/non-PIE 统一处理 |
| 内联符号解析 (snapshot API) | ✅ KernelSymbolResolver + ElfSymbolCache | 实时 UI 无需等管道 |
| 采样率背压降级 | ✅ 49→12 Hz (`OnBackpressure`) | perf_ebpf 无此机制 |

## 8. 尚未实现但值得学习的 perf_ebpf 功能

### 8.1 高优先级（P1 — 安全性 + 可靠性）

| 功能 | perf_ebpf 参考 | 对 illuminator 的价值 |
|------|----------------|---------------------|
| ~~**BPF skeleton 生成**~~ | ~~`bpf_rules.bzl` → `bpftool gen skeleton`~~ | ✅ **已实现** — `bpf_skeleton()` → `*.skel.h`，全部 6 个探针已迁移 |
| **min_duration_ns rodata 注入** | `skel->rodata->offcpu_min_duration_ns` | 用户 YAML 配置经 rodata 传入 BPF（当前用 map，可选优化） |
| **PID 动态发现** | `pid_manager_thread` 30s 周期 | 新启动的子进程自动纳入追踪 |
| **Buffer 溢出统计** | `meta_stats_map` + `MetaObserver` | /api/v1/budget 暴露丢数据指标 |
| **ResourceLimiter 强制执行** | （perf_ebpf 也缺少） | 超过 512MB 应自动停止采集 |

### 8.2 中优先级（P2 — 能力增强）

| 功能 | perf_ebpf 参考 | 对 illuminator 的价值 |
|------|----------------|---------------------|
| DWARF native unwinder | `native_unwinder.bpf.c` + `eh_frame_parser` | 支持无帧指针的商用二进制 |
| Map 跨 BPF 复用 | `bpf_map__reuse_fd()` | 减少内存、允许 sched+unwinder 协作 |
| v2 配置继承 (`extends`) | `config.cc` 多车型复用 | 多环境部署时减少配置冗余 |
| Session 管理 + 捕获轮转 | `SessionManager` | 长时间录制的磁盘管理 |
| per-CPU heap（避免 BPF 栈溢出） | `compat.bpf.h` 10KB per-CPU | 复杂事件的安全传递 |

### 8.3 低优先级（P3 — 锦上添花）

| 功能 | perf_ebpf 参考 | 说明 |
|------|----------------|------|
| Church 事件上报 | `ReporterManager` | illuminator 用 SSE + REST 已足够 |
| ftrace 管道 | `libtracefs` 集成 | 当前 eBPF 已覆盖需求 |
| D-state 独立阈值 | `dump_stack_threshold_ns` | 可区分正常阻塞和异常死锁 |

## 9. 对 illuminator 的具体改进建议

### 9.1 最值得借鉴的 3 个设计

**1. ~~Skeleton 生成 → 类型安全~~ ✅ 已完成**

所有 6 个 eBPF 探针已迁移到 skeleton API，使用 `skel_->maps.xxx` 类型安全访问。
`EbpfSkeletonSource` 基类 + `IL_DEFINE_SKEL_OPS` 宏提供统一的 skeleton 生命周期管理。

**2. rodata 配置注入**
```diff
- // 当前：C++ 读 YAML，BPF 用编译时常量
- const volatile u64 min_duration_ns = 10000000;  // BPF 侧永远是 10ms
+ // 改为：加载前设置
+ skel->rodata->min_duration_ns = config.min_duration_us * 1000;
```

**3. PID 动态刷新**
```diff
- // 当前：启动时写一次 target_pids map
+ // 改为：30s 周期扫描 /proc，更新 BPF PID map
+ // 新进程自动纳入，已退出进程自动移除
```

### 9.2 不建议照搬的 perf_ebpf 设计

| 设计 | 理由 |
|------|------|
| perf_event_array 替代 ringbuf | illuminator 的 ring_buffer 已工作良好，且内核 ≥5.8 场景下 ringbuf 更优 |
| 多进程 PluginManager 线程模型 | illuminator 的 InfrastructureManager (CollectPool+SinkPool) + per-Pipeline ProcessThread 更灵活 |
| JSON 配置格式 | YAML 对人类更友好，且已有完整解析层 |
| 文件输出为主 | illuminator 面向实时 UI，SSE+HTTP 是正确选择 |
