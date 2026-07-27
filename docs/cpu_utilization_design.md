# CPU 监控 Feature 设计方案

> 状态：v2（已拆分） | 作者：AI | 日期：2026-07-27

## 一、功能定位

CPU 监控拆分为**两个独立 Feature**，各自拥有独立的 Pipeline，在前端 CPU 页面通过**子标签页**分别展示。

| Feature | Name | 职责 | Tier |
|---------|------|------|------|
| CPU Utilization | `cpu_utilization` | 系统级 CPU 分布 + Per-Core + 负载 | Tier 1 Monitoring |
| Process CPU | `process_cpu` | 进程/线程级 CPU 占用 | Tier 1 Monitoring |

### 前端 CPU 页面结构（子标签页）

```
/feature/cpu
├── Tab: System      → cpu_utilization 数据
│   ├── 概览卡片（busy%, load avg, ctxt/s, run/blk）
│   ├── 堆叠面积图（user/system/iowait/irq/steal）
│   └── Per-Core 热力图
└── Tab: Processes   → process_cpu 数据
    ├── Top-N 进程表（可排序/搜索）
    └── 点击进程 → 线程级详情（QueryExtra 按需查询）
```

### 数据源

| 数据源 | 路径 | 所属 Feature | 读取方式 |
|--------|------|-------------|----------|
| CPU 时间分布 | `/proc/stat` | cpu_utilization | 差分计算 |
| 系统负载 | `/proc/loadavg` | cpu_utilization | 直接读取 |
| CPU 频率 | sysfs `cpufreq` | cpu_utilization | 直接读取（可选） |
| 进程 CPU | `/proc/[pid]/stat` | process_cpu | 差分计算 |
| 线程 CPU | `/proc/[pid]/task/[tid]/stat` | process_cpu | QueryExtra 按需 |
| 上下文切换 | `/proc/[pid]/status` | process_cpu | 直接读取 |

---

## 二、采集指标清单

### 2.1 系统级指标（来自 `/proc/stat` 差分）

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 用户态占比 | `user_pct` | double | % | 用户态进程消耗的 CPU 时间 |
| Nice 占比 | `nice_pct` | double | % | 低优先级用户态进程消耗 |
| 内核态占比 | `system_pct` | double | % | 内核态代码消耗 |
| 空闲占比 | `idle_pct` | double | % | CPU 空闲时间 |
| IO 等待占比 | `iowait_pct` | double | % | 等待 I/O 的空闲时间 |
| 硬中断占比 | `irq_pct` | double | % | 硬件中断处理 |
| 软中断占比 | `softirq_pct` | double | % | 网络收包等软中断 |
| Steal 占比 | `steal_pct` | double | % | 虚拟化环境中被 hypervisor 抢占 |
| **忙碌率** | `busy_pct` | double | % | = 100 - idle_pct - iowait_pct |

### 2.2 系统计数器（来自 `/proc/stat` 差分 → 速率）

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 上下文切换速率 | `ctxt_per_sec` | double | /s | 系统级上下文切换频率 |
| 中断速率 | `intr_per_sec` | double | /s | 系统级中断频率 |

### 2.3 运行队列状态（来自 `/proc/stat` 即时值）

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 运行进程数 | `procs_running` | uint32 | 个 | 当前处于 Running 状态的进程数 |
| 阻塞进程数 | `procs_blocked` | uint32 | 个 | 当前阻塞在 I/O 上的进程数 |

### 2.4 系统负载（来自 `/proc/loadavg`）

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 1 分钟负载 | `load_1m` | double | — | 过去 1 分钟的平均负载 |
| 5 分钟负载 | `load_5m` | double | — | 过去 5 分钟的平均负载 |
| 15 分钟负载 | `load_15m` | double | — | 过去 15 分钟的平均负载 |

### 2.5 Per-Core 指标（可选，来自 `/proc/stat` 各 cpuN 行）

每个 CPU 核心独立输出一条 Record，标签 `cpu=cpu0`，字段同 §2.1。

### 2.6 CPU 频率（可选，来自 sysfs）

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 当前频率 | `freq_mhz` | double | MHz | 每核当前运行频率 |

### 2.7 进程级指标（来自 `/proc/[pid]/stat` 差分）

按 `cpu_total_pct` 降序排列，保留 Top-N 个进程。

| 指标名 | 字段 | 类型 | 单位 | 说明 |
|--------|------|------|------|------|
| 进程 ID | label: `pid` | uint32 | — | |
| 进程名 | label: `comm` | string | — | |
| 用户态 CPU | `cpu_user_pct` | double | % | 该进程用户态 CPU 占比 |
| 内核态 CPU | `cpu_sys_pct` | double | % | 该进程内核态 CPU 占比 |
| **总 CPU** | `cpu_total_pct` | double | % | = user + sys |
| 进程状态 | `state` | string | — | R/S/D/Z/T |
| 线程数 | `num_threads` | uint32 | 个 | |
| RSS 内存 | `rss_kb` | uint64 | KB | 常驻物理内存 |
| 虚拟内存 | `vsize_kb` | uint64 | KB | 虚拟地址空间 |
| 主动上下文切换 | `vol_csw` | uint64 | 次 | 累计主动让出 CPU |
| 被动上下文切换 | `nonvol_csw` | uint64 | 次 | 累计被调度器抢占 |

### 2.8 线程级指标（按需查询，非定时采集）

线程级 CPU 数据**不在定时 Collect() 中自动采集**，而是通过 REST API 按需查询。
前端点击某个进程后，通过 `GET /api/v2/features/cpu_utilization/query?pid=<PID>` 触发
`Source::QueryExtra()`，返回 `/proc/<PID>/task/[tid]/stat` 的线程级详情。

响应格式：
```json
{
  "pid": 1234,
  "comm": "nginx",
  "threads": [
    { "tid": 1234, "comm": "nginx", "cpu_user_pct": 15.2, "cpu_sys_pct": 3.1, "state": "S" },
    { "tid": 1236, "comm": "worker-0", "cpu_user_pct": 8.5, "cpu_sys_pct": 1.2, "state": "R" }
  ]
}
```

---

## 三、数据批次格式

### 3.1 DataBatch 结构

一次 `Collect()` 调用返回**一个** `DataBatch(Type::kMetrics)`，内含多条 `Record`：

```
DataBatch {
  type: kMetrics
  meta: { "feature": "cpu_utilization", "interval_ms": "1000" }
  records: [
    // ① 系统总计 (1 条)
    { labels: [type="system_total"],
      fields: { user_pct, nice_pct, system_pct, idle_pct, iowait_pct,
                irq_pct, softirq_pct, steal_pct, busy_pct,
                ctxt_per_sec, intr_per_sec, procs_running, procs_blocked,
                load_1m, load_5m, load_15m } },

    // ② Per-Core (N 条，可选)
    { labels: [type="cpu_core", cpu="cpu0"],
      fields: { user_pct, system_pct, idle_pct, iowait_pct, busy_pct, freq_mhz } },
    { labels: [type="cpu_core", cpu="cpu1"], ... },

    // ③ Top-N 进程 (最多 N 条)
    { labels: [type="process", pid="1234", comm="nginx"],
      fields: { cpu_user_pct, cpu_sys_pct, cpu_total_pct, state,
                num_threads, rss_kb, vsize_kb, vol_csw, nonvol_csw } },

    // ④ 线程级数据不在定时采集中包含，通过 QueryExtra API 按需查询
  ]
}
```

### 3.2 SSE JSON 格式（经 SseSink 序列化）

```json
{
  "feature": "cpu_utilization",
  "modelType": "time_series",
  "timestamp": 1719900000000,
  "seq": 42,
  "metrics": [
    {
      "labels": { "type": "system_total" },
      "fields": {
        "user_pct": 23.5,
        "system_pct": 12.1,
        "idle_pct": 55.2,
        "iowait_pct": 3.1,
        "busy_pct": 41.7,
        "load_1m": 2.34,
        "ctxt_per_sec": 15234,
        "procs_running": 5
      },
      "timestamp": 1719900000000
    },
    {
      "labels": { "type": "process", "pid": "1234", "comm": "nginx" },
      "fields": {
        "cpu_total_pct": 34.2,
        "cpu_user_pct": 28.1,
        "cpu_sys_pct": 6.1,
        "num_threads": 16,
        "rss_kb": 524288
      },
      "timestamp": 1719900000000
    }
  ]
}
```

---

## 四、可配置项

### 4.1 采集参数

| 配置项 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `interval_ms` | int | 1000 | 200~10000 | 采集间隔（毫秒） |
| `collect_per_core` | bool | true | — | 是否采集每核数据 |
| `collect_frequency` | bool | false | — | 是否采集 CPU 频率 |
| `collect_processes` | bool | true | — | 是否采集进程级 CPU |
| `top_n` | int | 20 | 5~100 | 保留的 Top-N 高 CPU 进程数 |
| `ema_alpha` | double | 0.0 | 0~1 | 指数移动平均平滑系数（0=不平滑） |

### 4.2 过滤参数

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `pid_filter` | list[int] | [] | 仅采集指定 PID（空=全部） |
| `comm_filter` | string | "" | 进程名正则匹配（空=全部） |
| `exclude_kernel_threads` | bool | true | 排除内核线程（PID 2 的子进程） |
| `min_cpu_threshold` | double | 0.1 | 进程 CPU% 低于此值时不上报 |

### 4.3 REST API 运行时配置

```
POST /api/v2/features/cpu_utilization/config
{
  "interval_ms": 2000,
  "collect_per_core": false,
  "top_n": 10,
  "comm_filter": "^(nginx|redis|mysql)"
}
```

### 4.4 ConfigSchema（JSON Schema 给前端渲染配置面板）

```json
{
  "type": "object",
  "properties": {
    "interval_ms":   { "type": "integer", "default": 1000, "minimum": 200, "maximum": 10000, "description": "采集间隔 (ms)" },
    "collect_per_core": { "type": "boolean", "default": true, "description": "采集每核心数据" },
    "collect_frequency": { "type": "boolean", "default": false, "description": "采集 CPU 频率" },
    "collect_processes": { "type": "boolean", "default": true, "description": "采集进程级 CPU" },
    "top_n":         { "type": "integer", "default": 20, "minimum": 5, "maximum": 100, "description": "Top-N 进程数" },
    "ema_alpha":     { "type": "number", "default": 0, "minimum": 0, "maximum": 1, "description": "EMA 平滑系数" },
    "comm_filter":   { "type": "string", "default": "", "description": "进程名正则过滤" },
    "exclude_kernel_threads": { "type": "boolean", "default": true, "description": "排除内核线程" },
    "min_cpu_threshold": { "type": "number", "default": 0.1, "minimum": 0, "maximum": 100, "description": "最小上报阈值 (%)" }
  }
}
```

---

## 五、前端图表设计

### 5.1 页面结构

```
┌─────────────────────────────────────────────────────┐
│  CPU Utilization   ● active  [Pause] [Config]       │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ │
│  │ busy%    │ │ load avg │ │ ctxt/s   │ │ run/blk│ │
│  │  41.7%   │ │  2.34    │ │  15.2k   │ │  5 / 0 │ │
│  │ ~spark~  │ │ ~spark~  │ │ ~spark~  │ │ ~spark~│ │
│  └──────────┘ └──────────┘ └──────────┘ └────────┘ │
│                                                     │
│  ┌─────────────────────────────────────────────────┐│
│  │  ████ Stacked Area Chart (5min window)          ││
│  │  ████  user | system | iowait | irq | steal    ││
│  │  ████████████████████████████████████████████   ││
│  │  ════════════════════════════════════════════   ││
│  │              DataZoom slider                    ││
│  └─────────────────────────────────────────────────┘│
│                                                     │
│  ┌─────────────────────────────────────────────────┐│
│  │  Per-Core Heatmap (optional, when enabled)      ││
│  │  cpu0 ████████░░  78%                           ││
│  │  cpu1 ██████░░░░  62%                           ││
│  │  cpu2 ████░░░░░░  41%                           ││
│  │  cpu3 ██████████  95%                           ││
│  └─────────────────────────────────────────────────┘│
│                                                     │
│  ┌─────────────────────────────────────────────────┐│
│  │  Top Processes                    Sort: CPU% ▼  ││
│  ├──────┬──────┬───────┬──────┬──────┬──────┬─────┤│
│  │ PID  │ Name │ CPU%  │ User │ Sys  │ Thrd │ RSS ││
│  ├──────┼──────┼───────┼──────┼──────┼──────┼─────┤│
│  │ 1234 │nginx │ 34.2% │ 28.1 │  6.1 │  16  │512M ││
│  │  567 │redis │ 12.8% │ 10.2 │  2.6 │   4  │256M ││
│  │ ...  │ ...  │  ...  │ ...  │  ... │  ... │ ... ││
│  └──────┴──────┴───────┴──────┴──────┴──────┴─────┘│
│                                                     │
│  [Raw Data]  [Configuration]                        │
└─────────────────────────────────────────────────────┘
```

### 5.2 顶部概览卡片（4 个 StatCard）

| 卡片 | 主值 | 单位 | SparkLine 数据 | 阈值 |
|------|------|------|----------------|------|
| CPU Busy | `busy_pct` | % | 最近 60 个 busy_pct 值 | warn=70%, crit=90% |
| Load Average | `load_1m` | — | 最近 60 个 load_1m 值 | warn=核心数×0.8, crit=核心数 |
| Context Switch | `ctxt_per_sec` | /s | 最近 60 个值 | 无固定阈值 |
| Run/Blocked | `procs_running/procs_blocked` | 个 | 最近 60 个 running 值 | — |

### 5.3 主图表：CPU 分布堆叠面积图

- **类型**：ECharts Stacked Area Chart
- **系列**：`user_pct`（indigo）、`system_pct`（orange）、`iowait_pct`（red）、`irq_pct+softirq_pct`（yellow）、`steal_pct`（purple）
- **Y 轴**：0-100%，固定范围
- **X 轴**：时间轴，默认 5 分钟窗口（300 个数据点 @1s 间隔）
- **DataZoom**：底部滑块，可调整时间范围
- **阈值线**：`busy_pct` 70% (warning, 虚线黄)、90% (critical, 虚线红)
- **Tooltip**：显示该时间点所有系列的值 + busy_pct 总计

### 5.4 Per-Core 热力图（可选面板）

- **类型**：横向条形图 / 热力方格
- **行**：每个 CPU 核心一行
- **颜色映射**：0%→深蓝, 50%→蓝, 70%→黄, 90%→红
- **显示**：`busy_pct` 百分比数字 + 颜色条
- **默认折叠**：用户点击 "Per-Core" 标签展开
- **排序**：按 CPU ID 或按 busy_pct 降序

### 5.5 进程表

- **列**：PID, Name, CPU%, User%, Sys%, State, Threads, RSS, Vol CSW, Nonvol CSW
- **排序**：默认按 CPU% 降序，点击列头切换
- **行内 SparkLine**：CPU% 列可选择显示迷你趋势图（需在前端缓存 60 个历史值）
- **展开行**：高 CPU 进程可点击展开查看线程级详情
- **状态着色**：R→绿, S→灰, D→红(不可中断), Z→紫(僵尸)
- **搜索**：支持按进程名模糊搜索

### 5.6 底部 Tab 切换

| Tab | 内容 |
|-----|------|
| Raw Data | JSON 原始数据展示（可折叠） |
| Configuration | 基于 ConfigSchema 自动渲染的配置表单 |
| Load Average | 独立的 1m/5m/15m 趋势线图 |

---

## 六、后端实现架构

### 6.1 文件结构

```
src/plugin/features/cpu/cpu_utilization/
├── cpu_utilization_source.h     # SourcePlugin 实现
├── cpu_utilization_driver.h     # FeatureDriver 实现
└── BUILD                        # Bazel 构建规则
```

### 6.2 数据流

```
TimerWheel (1s)
  → CollectPool → CpuUtilizationSource::Collect()
      → proc::ReadCpuSnapshot()        ← /proc/stat
      → proc::ReadLoadAvg()            ← /proc/loadavg
      → proc::ScanProcesses()          ← /proc/[pid]/stat (可选)
      → proc::ReadCpuFrequenciesMhz()  ← sysfs (可选)
      → 差分计算 → DataBatch
  → AsyncChannel
  → ProcessThread (直通，无 Processor)
  → SinkPool → SseSink (JSON → SSE)
               → RecordingSink (JSON → .ilm, 可选)
```

### 6.3 差分计算逻辑

```
dt = cur.cores[0].Total() - prev.cores[0].Total()
if dt == 0: skip (避免除零)

user_pct   = (cur.user   - prev.user)   / dt * 100
system_pct = (cur.system - prev.system) / dt * 100
idle_pct   = (cur.idle   - prev.idle)   / dt * 100
...
busy_pct   = 100 - idle_pct - iowait_pct

ctxt_per_sec = (cur.ctxt - prev.ctxt) / (dt_real_sec)
intr_per_sec = (cur.intr - prev.intr) / (dt_real_sec)
```

### 6.4 进程 CPU 计算逻辑

```
total_delta = ReadTotalCpuJiffies() - prev_total_jiffies
per_process:
  proc_delta = (cur.utime + cur.stime) - (prev.utime + prev.stime)
  cpu_total_pct = proc_delta / total_delta * 100 * num_cpus
  cpu_user_pct  = (cur.utime - prev.utime) / total_delta * 100 * num_cpus
  cpu_sys_pct   = (cur.stime - prev.stime) / total_delta * 100 * num_cpus
```

---

## 七、Overview 页面集成

在 Overview 仪表盘中，cpu_utilization 作为 Golden Signal 的 **Saturation** 指标展示：

| 位置 | 展示 | 数据来源 |
|------|------|----------|
| StatCard "CPU" | `busy_pct` 值 + SparkLine | `system_total.busy_pct` |
| FeatureCard | "CPU Utilization" + "78% avg" + SparkLine | `system_total.busy_pct` |

---

## 八、开发计划

| 阶段 | 内容 | 工时估计 |
|------|------|----------|
| P1 | `cpu_utilization_source.h` — 系统级指标采集 | 2h |
| P2 | `cpu_utilization_source.h` — 进程级采集 + Top-N | 2h |
| P3 | `cpu_utilization_driver.h` + BUILD + 注册 | 1h |
| P4 | 编译验证 + 基本功能测试 | 1h |
| P5 | 前端 SSE 对接 + 实时图表渲染 | 4h |
| P6 | Per-Core 热力图 + 进程表交互 | 3h |
| P7 | 配置面板 + REST API 对接 | 2h |

**总计约 15 小时**，建议先完成 P1-P4（后端闭环），再做 P5-P7（前端对接）。

---

## 九、讨论点

1. **进程采集已拆分为独立 Feature**
   - ✅ 已决定：`cpu_utilization`（系统级）+ `process_cpu`（进程级）
   - 前端 CPU 页面使用子标签页（System / Processes）分别展示

2. **线程级数据**
   - 已确定：线程级数据通过 REST API 按需查询（前端点击进程时触发）
   - 实现方式：Source::QueryExtra("threads", {pid=1234}) → 读取 /proc/[pid]/task/*/stat

3. **历史数据存储？**
   - 当前仅 SSE 实时推送，无持久化
   - 可选：通过 RecordingSink 写入 `.ilm` 文件，或后续接入 SQLite

4. **EMA 平滑是否必要？**
   - 1s 采集间隔下波动较大，EMA(alpha=0.3) 可平滑抖动
   - 但平滑会掩盖瞬时尖刺，是否由前端侧做更好？
