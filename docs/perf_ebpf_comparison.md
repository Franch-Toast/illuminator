# perf_ebpf vs illuminator 架构对比分析

## 1. 项目定位对比

| 维度 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **定位** | 车端生产级性能观测守护进程 | 通用高性能可观测平台 |
| **目标环境** | QNX/Linux 嵌入式（多车型） | Linux 服务器/容器 |
| **eBPF 插件数** | 14 个（含 DWARF 展开） | 4 个（sched/offcpu/cpu/bio） |
| **构建系统** | Bazel + CMake（辅助） | Bazel |
| **数据输出** | 文件 + Church 事件上报 | WebSocket 实时推送 + SQLite |
| **配置格式** | JSON（v1/v2 两套，支持继承） | YAML（单配置文件） |

## 2. 架构差异

### 2.1 数据流

```
perf_ebpf:
  BPF → perf_event_array/ringbuf → 插件线程直接处理 → Sink(文件)
  特点：路径短，无中间队列

illuminator:
  BPF → ringbuf → Source → AsyncChannel → Processor → SinkPool → WebSocket/SQLite
  特点：灵活组合，但链路长，背压传递慢
```

### 2.2 插件体系

| 方面 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **注册** | `REGISTER_PLUGIN` 静态构造 | `IL_REGISTER_SOURCE/PROCESSOR/SINK` |
| **生命周期** | init → start → stop → rotate | Init → Start → Stop |
| **线程模型** | 每插件独立轮询线程 | PipelineController 统一调度 |
| **探针管理** | 插件自管理 skeleton | BpfProgramManager 统一加载 |

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

## 5. 编译选项对比

| 选项 | perf_ebpf | illuminator |
|------|-----------|-------------|
| **C++ 标准** | C++17 | C++20 |
| **优化级别** | 框架 -O2，主程序 -O0 | -O2（Bazel 默认） |
| **调试符号** | `-g` 保留 | 需 `--strip=never` |
| **帧指针** | 不强制（DWARF 展开不需要） | 需 `-fno-omit-frame-pointer`（FP 展开依赖） |
| **BPF 编译** | `clang-14 -g -O2 -target bpf` | `clang -g -O2 -target bpf` |

**关键差异**：perf_ebpf 的 DWARF 展开路径不依赖 `-fno-omit-frame-pointer`，而 illuminator
当前仅支持 FP-based 栈展开（`bpf_get_stackid(BPF_F_USER_STACK)`），因此**必须**确保目标
二进制带帧指针。

## 6. 安全机制总结

perf_ebpf **不冻结系统**的根本原因（按重要性排序）：

1. **off-CPU 仅对 ≤16 个目标 TGID 做用户栈展开** — 非目标进程的 sched_switch 开销极低
2. **启动延迟期 PID map 为空** — 等价于 off-CPU 完全关闭，系统先稳定
3. **内核 map 聚合 + 1Hz 信号** — sched 热路径不做 ringbuf 分配
4. **perf_event_array 溢出丢弃** — 不阻塞 BPF 执行
5. **1ms 阈值（非 100μs）** — 过滤掉大量正常调度切换
6. **内核线程跳过** — kworker 等高频切换的线程不做 USER_STACK

## 7. illuminator v3.1 已学习并实现的 perf_ebpf 机制

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

## 8. 尚未实现但值得学习的 perf_ebpf 功能（P1/P2）

| 功能 | 优先级 | perf_ebpf 参考 |
|------|--------|----------------|
| PID 动态发现 (`pid_manager_thread`) | P1 | 自动追踪新进程，无需重启 |
| DWARF native unwinder | P1 | 无帧指针二进制的深栈解析 |
| Buffer 溢出统计 (`meta_stats_map`) | P1 | 运维可见的丢数据指标 |
| BPF skeleton 全面使用 | P2 | 类型安全、减少 raw fd 操作 |
| Session 管理 + 数据轮转 | P2 | 长时间运行的内存/磁盘管理 |
