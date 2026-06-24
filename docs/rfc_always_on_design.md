# RFC: Always-On 架构设计方案

> **状态**: Implemented (Phase 1-3)  
> **作者**: Illuminator Architecture Team  
> **日期**: 2026-06-24  
> **目标**: 将 Illuminator 前端从"隐式激活"模式改为"永远在线 + 按需 Profiling"模式

---

## 一、设计哲学

**核心理念**：前端是纯粹的**数据查看器**，不负责管理后端采集生命周期。

```
当前模式（问题）:
  用户打开页面 → 前端 POST /features/start → 后端开始采集 → 数据送达

Always-On 模式（目标）:
  Daemon 启动 → 自动运行所有 Monitoring Feature → 数据持续产生
  用户打开页面 → 前端订阅数据流 → 立即看到数据
```

**对标**：Grafana + Prometheus node_exporter 模式
- node_exporter 永远采集 CPU/Memory/Disk/Network 指标
- Grafana Dashboard 打开即有数据，关闭 Dashboard 不影响采集
- 用户永远不需要"启动"一个基础指标

---

## 二、架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Illuminator Daemon                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────────────────────┐  ┌─────────────────────┐   │
│  │    Always-On Layer (Tier 1-2)        │  │  On-Demand Layer     │   │
│  │                                      │  │  (Tier 3 Profiling)  │   │
│  │  cpu_utilization   ← 1s interval    │  │                     │   │
│  │  cpu_processes     ← 2s interval    │  │  cpu_profile         │   │
│  │  memory_utilization← 1s interval    │  │  offcpu_profile      │   │
│  │  io_monitor        ← event-driven   │  │  sched_analysis      │   │
│  │  net_tracer        ← event-driven   │  │                     │   │
│  │  gpu_monitor       ← 1s interval    │  │  需 POST /start     │   │
│  │                                      │  │  需指定 target_pids  │   │
│  │  Daemon 启动即运行                   │  │  有明确结束时间      │   │
│  │  无需前端触发                        │  │                     │   │
│  └─────────────────────────────────────┘  └─────────────────────┘   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │                    HTTP + WebSocket Server (:9527)                 │ │
│  │  GET  /api/v1/features → 列出所有 feature 状态                   │ │
│  │  GET  /api/v1/features/:name/collect → 读取最新数据              │ │
│  │  GET  /api/v1/features/:name/stream  → cursor 增量拉取          │ │
│  │  POST /api/v1/sessions/start → 创建 Profiling Session           │ │
│  │  POST /api/v1/sessions/:id/stop → 停止 Session                  │ │
│  │  WS   /ws/features → 实时推送所有活跃 feature 数据              │ │
│  └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                        Web Frontend (纯查看器)                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ Dashboard │  │  CPU     │  │ Profiling │  │  Replay  │            │
│  │ (概览)    │  │ Memory   │  │ Sessions  │  │ (历史)   │            │
│  │           │  │ IO/Net   │  │           │  │          │            │
│  │ 打开即有  │  │ 打开即有 │  │ 需创建   │  │ 需加载   │            │
│  │ 数据      │  │ 数据     │  │ Session   │  │ 文件     │            │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘            │
│                                                                       │
│  不再调用 POST /features/start (Tier 1-2)                            │
│  仅订阅数据：getDataSource().subscribe(feature, callback)            │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 三、后端修改

### 3.1 启动流程变更

**当前**：`main.cc` 注册 Feature → `FeatureManager` 标记为 `inactive` → 等待前端 POST start

**改为**：

```cpp
// main.cc 中：
for (const auto& feature : feature_manager.ListFeatures()) {
    if (feature.tier <= 2) {  // Tier 1-2 自动启动
        FeatureManager::StartParams params;
        params.auto_started = true;
        feature_manager.Start(feature.name, params);
    }
}
IL_INFO("Always-on features started: {}", auto_started_count);
```

### 3.2 配置文件控制

```yaml
# illuminator.yaml
always_on:
  enabled: true                    # 全局开关（嵌入式低资源环境可设为 false）
  features:
    - cpu_utilization              # 可精确指定哪些 Feature 自动启动
    - cpu_processes
    - memory_utilization
    - io_monitor
    - net_tracer
    # - gpu_monitor              # 注释掉 = 不自动启动
  
  # 或使用 tier 过滤（与上面二选一）
  # auto_start_tier: 2           # 自动启动 tier <= 2 的所有 feature

profiling:
  max_concurrent_sessions: 2       # 同时最多 2 个 Profiling Session
  default_duration_sec: 60         # 默认持续时间
  auto_stop: true                  # 超时自动停止
```

### 3.3 API 变更

| 变更 | 旧 API | 新 API |
|------|--------|--------|
| **删除** | `POST /api/v1/features/:name/start` (Tier 1-2) | — |
| **删除** | `POST /api/v1/features/:name/stop` (Tier 1-2) | — |
| **保留** | `POST /api/v1/features/:name/start` (Tier 3) | 改为 `POST /api/v1/sessions` |
| **新增** | — | `GET /api/v1/sessions` (列出活跃 Session) |
| **新增** | — | `POST /api/v1/sessions/:id/stop` |
| **保留** | `POST /api/v1/features/:name/pause` | 保留但仅用于调试/高级场景 |

**新 Session API 设计**：

```
POST /api/v1/sessions
{
  "type": "cpu_profile",           // 或 "offcpu_profile", "sched_analysis"
  "target_pids": [1234, 5678],
  "target_comms": ["illuminator"],
  "duration_sec": 30,              // 可选，0 = 无限直到手动停止
  "options": {
    "frequency_hz": 49
  }
}

Response:
{
  "session_id": "sess_abc123",
  "status": "active",
  "started_at": "2026-06-24T08:00:00Z",
  "expires_at": "2026-06-24T08:00:30Z"
}
```

### 3.4 录制改为"导出"模式

**当前**：录制是"从现在开始写文件直到停止"——如果 Feature 没启动就录不到。

**改为**：数据始终在内存环形缓冲区中（StreamSinkStore 已有此能力）。"导出"是将缓冲区中的历史数据 + 实时数据写入文件。

```
POST /api/v1/export/start
{
  "features": ["cpu_utilization", "cpu_processes", "memory_utilization"],
  "lookback_sec": 300,             // 回溯 5 分钟的历史数据
  "format": "ilr"                  // 或 "ndjson"
}

POST /api/v1/export/stop
→ { "file": "/data/illuminator/exports/2026-06-24_080000.ilr", "bytes": 12345 }
```

**用户视角变化**：
- 旧："我必须先开录制，再触发问题"
- 新："问题已经发生了，我导出过去 5 分钟的数据来分析"

---

## 四、前端修改

### 4.1 移除 `usePageActivation` 和 `useFeaturesByCategory`

这两个 hook 不再需要。页面打开时不需要调用 `featureStart`。

```diff
// 删除：
- import { usePageActivation, useFeaturesByCategory } from '../hooks/useDataSource'
- useFeaturesByCategory('cpu')  // 不再需要
- usePageActivation('cpu', features)  // 不再需要

// 保留：
+ const { data } = useCpuUtilization(active)  // 直接订阅，数据始终存在
```

### 4.2 简化数据 Hook

数据 Hook 不再需要检查"Feature 是否已启动"——数据永远在线：

```typescript
// 新版 useCpuUtilization（简化）
export function useCpuUtilization() {
  const mode = useTimeStore(s => s.mode)
  const [data, setData] = useState<CpuUtilizationPoint[]>([])

  useEffect(() => {
    if (mode === 'paused') return
    const source = getDataSource()
    return source.subscribe('cpu_utilization', (batch) => {
      setData(prev => [...prev.slice(-59), parseBatch(batch)])
    })
  }, [mode])

  return data
}
```

移除的参数：`active`（不再需要，数据总在）、`intervalMs`（后端决定）。

### 4.3 新增 "数据可用性" 指示

虽然不需要启动 Feature，但需要处理后端未运行或 Feature 异常的情况：

```typescript
// 新 hook：useFeatureHealth
export function useFeatureHealth(featureName: string) {
  const [health, setHealth] = useState<'active' | 'degraded' | 'unavailable'>('active')
  
  useEffect(() => {
    // 订阅 Feature 状态变化通知
    const source = getDataSource()
    const timeout = setTimeout(() => {
      // 5 秒内没收到数据 → 可能有问题
      setHealth('degraded')
    }, 5000)
    
    const unsub = source.subscribe(featureName, () => {
      clearTimeout(timeout)
      setHealth('active')
    })
    return () => { clearTimeout(timeout); unsub() }
  }, [featureName])

  return health
}
```

UI 展示：
```
┌─────────────────────────────────────────────────────┐
│  CPU Utilization  ●  (绿色 = 数据正常流入)          │
│  ┌─────────────────────────────────────────────┐    │
│  │  ████████████████████████████               │    │
│  │  ██  CPU chart (auto-fills)  ██             │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘

// 异常时：
┌─────────────────────────────────────────────────────┐
│  CPU Utilization  ⚠ (黄色 = 数据停止)               │
│  ┌─────────────────────────────────────────────┐    │
│  │  ⚠ No data received for 10s.                │    │
│  │  Check daemon status or eBPF availability.  │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
```

### 4.4 Profiling 改为 "Session" UI

```typescript
// 新组件：ProfilingSession
function ProfilingSession({ pid, comm }: { pid: number; comm: string }) {
  const [session, setSession] = useState<Session | null>(null)

  const start = async () => {
    const resp = await api.createSession({
      type: 'cpu_profile',
      target_pids: [pid],
      duration_sec: 30,
    })
    setSession(resp)
  }

  const stop = async () => {
    if (session) await api.stopSession(session.session_id)
    setSession(null)
  }

  return (
    <div>
      {!session && (
        <button onClick={start}>
          Start 30s CPU Profile for {comm} (PID {pid})
        </button>
      )}
      {session && (
        <div>
          <StatusBadge status={session.status} />
          <Timer startedAt={session.started_at} />
          <button onClick={stop}>Stop Early</button>
          {session.status === 'complete' && <FlameGraph sessionId={session.session_id} />}
        </div>
      )}
    </div>
  )
}
```

### 4.5 录制 → 导出 UI 重设计

```typescript
// 新组件：ExportControl（替换 RecordingControl）
function ExportControl() {
  const [exporting, setExporting] = useState(false)
  const [lookback, setLookback] = useState(300) // 5分钟

  const startExport = async () => {
    setExporting(true)
    await api.exportStart({
      features: ['cpu_utilization', 'cpu_processes', 'memory_utilization'],
      lookback_sec: lookback,
    })
  }

  return (
    <div>
      {!exporting ? (
        <>
          <label>导出最近：
            <select value={lookback} onChange={e => setLookback(+e.target.value)}>
              <option value={60}>1 分钟</option>
              <option value={300}>5 分钟</option>
              <option value={900}>15 分钟</option>
              <option value={1800}>30 分钟</option>
            </select>
          </label>
          <button onClick={startExport}>📦 导出为 .ilr</button>
        </>
      ) : (
        <ExportProgress onComplete={() => setExporting(false)} />
      )}
    </div>
  )
}
```

### 4.6 PluginManager 页面变化

从"启动/停止"管理界面改为"健康监控"界面：

| 旧功能 | 新功能 |
|--------|--------|
| Start / Stop / Pause / Resume | 仅显示状态；Stop 仅用于紧急场景 |
| Start All | 不需要（已自动启动） |
| Tier 标签 | 改为 "Always-On" 和 "On-Demand" 分组 |
| Action Log | 保留：显示自动启动/错误日志 |
| 资源预算 | 保留并增强：显示各 Feature 的 CPU/内存占用 |

---

## 五、迁移策略

### Phase 1（向后兼容过渡）

1. 后端配置新增 `always_on.enabled: true`（默认 false，不影响现有行为）
2. 设为 true 时，daemon 启动自动运行 Tier 1-2
3. 前端 `usePageActivation` 保留但变为 no-op（如果 Feature 已 active 则 start 是幂等的）
4. 前端检测到 Feature 已 active 时跳过 start 调用

### Phase 2（前端简化）

1. 移除 `usePageActivation` 和 `useFeaturesByCategory`
2. 数据 Hook 移除 `active` 参数
3. 新增 `useFeatureHealth` hook
4. 页面组件简化

### Phase 3（API 演进）

1. Tier 1-2 的 `POST /start` 和 `POST /stop` 标记 Deprecated
2. 新增 `/api/v1/sessions` API for Tier 3
3. 新增 `/api/v1/export` API 替代 `/api/v1/recording`

### Phase 4（UI 重构）

1. RecordingControl → ExportControl
2. ProfilingSection → ProfilingSession 组件
3. PluginManager → FeatureHealthDashboard

---

## 六、资源考量（嵌入式/车端场景）

### 问题：Always-On 会不会太耗资源？

**分析当前 Tier 1-2 的实际开销：**

| Feature | 机制 | CPU 开销 | 内存开销 |
|---------|------|---------|---------|
| cpu_utilization | 读 /proc/stat，1s | < 0.1% | ~50KB |
| cpu_processes | 读 /proc/[pid]/stat，2s | < 0.5%（取决于进程数） | ~200KB |
| memory_utilization | 读 /proc/meminfo，1s | < 0.01% | ~10KB |
| io_monitor | eBPF tracepoint，事件驱动 | < 0.5%（取决于 IOPS） | ~256KB (ringbuf) |
| net_tracer | eBPF kprobe，事件驱动 | < 0.5%（取决于包率） | ~256KB (ringbuf) |

**总 CPU 开销**：< 2%（正常负载下）  
**总内存开销**：< 2MB  

**对比**：
- Prometheus node_exporter 常驻内存 ~20MB，CPU ~1%
- Datadog Agent 常驻内存 ~100MB，CPU ~2%
- perf_ebpf 车端运行全功能 CPU ~5%

**结论**：Tier 1-2 Always-On 的资源开销是可接受的，即使在嵌入式场景。

### 低资源模式（可选）

```yaml
always_on:
  enabled: true
  low_power_mode: true    # 降低频率
  intervals:
    cpu_utilization: 5000  # 5s 代替 1s
    cpu_processes: 10000   # 10s 代替 2s
```

---

## 七、用户体验对比

### 7.1 监控场景

| 步骤 | 当前 | Always-On 方案 |
|------|------|---------------|
| 1 | 打开 app → Overview（空） | 打开 app → Overview（有 CPU/Mem 摘要） |
| 2 | 点 CPU → 等 1-3s | 点 CPU → **立即**有数据 |
| 3 | 切到 Memory → 等 1-3s | 切到 Memory → **立即**有数据 |
| 4 | 回到 CPU → 数据连续 | 回到 CPU → 数据连续 |
| 5 | 关闭浏览器 | 关闭浏览器 |
| 6 | 后端：5 组 Feature 继续运行 | 后端：Same（但这是预期行为） |

### 7.2 录制/导出场景

| 步骤 | 当前 | Always-On 方案 |
|------|------|---------------|
| 1 | 先访问 CPU/Mem/IO 页 | 不需要 |
| 2 | 点击"录制" | 点击"导出最近 5 分钟" |
| 3 | 等待问题发生... | **问题已经发生了也能捕获** |
| 4 | 停止录制 | 导出完成，下载 .ilr 文件 |
| 5 | 去 Replay 页加载 | 去 Replay 页加载（或直接查看） |

### 7.3 Profiling 场景

| 步骤 | 当前 | Always-On 方案 |
|------|------|---------------|
| 1 | CPU → Process → 选 PID | CPU → Process → 选 PID |
| 2 | "Start CPU Profile" | "Start 30s Profile Session" |
| 3 | 等待采样... | 等待采样... |
| 4 | 查看火焰图 | 查看火焰图 |
| 5 | **离开页面后 Profiling 继续** | **Session 30s 后自动停止** |
| 6 | 必须去 PluginManager 手动停止 | 自动停止（或提前点 Stop） |

---

## 八、与现有 Replay 的整合

### 当前 Replay 的问题

- 独立路由 `/replay`，与 Live 完全割裂
- 用户必须手动加载文件
- 部分 Feature 无 Replay 视图（IO/Net/GPU 显示 "coming soon"）

### Always-On 方案下的 Replay 改进

**导出即 Replay 源**：

```
Dashboard → "导出最近 5 分钟" → 自动弹出 Replay 视图（无需手动加载文件）
```

**Session 完成即 Replay**：

```
Profiling Session 完成 → "View Results" → 直接展示火焰图 + 时间线
                        ↘ "Export .ilr" → 下载供离线分析
```

**统一时间线**：

```
┌──────────────────────────────────────────────────────────────┐
│  Time Ruler: ◄ 08:00    08:15    08:30    08:45    09:00 ►  │
│                                                   ▲ NOW      │
│  CPU █████████████████████████████████████████████████████    │
│  MEM ████████████████████████████████████████████████████    │
│                                                              │
│  [Session #3: CPU Profile]  ████                            │
│                             08:20 - 08:20:30                 │
│                                                              │
│  [Export #1]  ██████████████████████████                     │
│               08:00 - 08:30                                  │
└──────────────────────────────────────────────────────────────┘
```

---

## 九、文件变更清单（预估）

### 后端

| 文件 | 变更 |
|------|------|
| `src/cli/main.cc` | 启动后自动 Start Tier 1-2 |
| `src/core/engine/feature_manager.h` | 新增 `AutoStartFeatures()` 方法 |
| `src/server/api_routes.h` | 新增 `/sessions` API；标记 Tier 1-2 start/stop 为 deprecated |
| `config/illuminator.yaml.example` | 新增 `always_on` 配置段 |

### 前端

| 文件 | 变更 |
|------|------|
| `hooks/useDataSource.ts` | 移除 `usePageActivation`、`useFeaturesByCategory` |
| `hooks/useCpuData.ts` | 简化：移除 `active` 参数和 Feature 启动逻辑 |
| `hooks/useMemoryData.ts` | 同上 |
| `hooks/useIoData.ts` | 同上 |
| `hooks/useNetworkData.ts` | 同上 |
| `hooks/useGpuData.ts` | 同上 |
| `hooks/useFeatureHealth.ts` | **新增**：数据可用性检测 |
| `pages/CpuPage.tsx` | 移除 activation 调用 |
| `pages/MemoryPage.tsx` | 同上 |
| `pages/IoPage.tsx` | 同上 |
| `pages/NetworkPage.tsx` | 同上 |
| `pages/GpuPage.tsx` | 同上 |
| `pages/OverviewPage.tsx` | 新增 key metrics 摘要（从 WS 实时获取） |
| `components/Layout/RecordingControl.tsx` | **重写**为 ExportControl |
| `components/charts/ProfileSnapshot.tsx` | 改为 Session 模型 |
| `services/apiClient.ts` | 新增 session/export API |

---

## 十、风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| eBPF 探针自动加载失败（权限不足） | 启动日志明确提示；前端 Health 指示器变黄 |
| 高负载下 IO/Net 事件风暴 | 后端已有背压（49→12Hz）；Always-On 不改变此机制 |
| 嵌入式环境不想自动采集 | `always_on.enabled: false` 配置回退到当前行为 |
| 前端改动大，可能引入回归 | Phase 1 兼容模式；旧 API 保留但 deprecated |
| 数据保留时间占内存 | 环形缓冲区大小可配（默认 30 分钟 retention 已实现） |

---

## 十一、成功指标

1. **首次数据展示时间**: 从 1-3s 降至 < 100ms（仅 WebSocket 订阅延迟）
2. **用户操作步骤**: 从"导航 → 等待 → 看数据"简化为"导航 → 看数据"
3. **录制成功率**: 从"必须先访问页面"改为"任何时候都能导出最近 N 分钟"
4. **Profiling 会话泄漏**: 从"永不停止"改为"自动过期"
5. **前端代码量**: 预计减少 ~200 行（移除 activation 逻辑）
6. **新页面开发成本**: 从"5 步"减至"2 步"（写 Hook + 写 UI）

---

## 十二、开放问题

1. **GPU Feature 在无 GPU 环境下自动启动是否浪费？**
   → 通过 FeatureProbe 检测硬件，无 GPU 则不注册该 Feature

2. **Tier 2（IO/Net）的 eBPF 探针是否默认开启太重？**
   → 取决于目标场景。车端可能仅默认 Tier 1，Tier 2 配置启用

3. **Session 的数据如何持久化？**
   → 复用现有 StreamSinkStore + SQLite；Session 完成后数据自动保留至 retention 期

4. **多用户场景（多个浏览器标签）如何处理 Session 冲突？**
   → Session 有 `max_concurrent_sessions` 限制；返回 409 Conflict

---

## 附录：与当前 perf_ebpf 的对比

perf_ebpf 已经是 "Always-On" 模式：
- Daemon 启动 → 自动运行所有配置的插件
- 无 HTTP 前端，不需要"启动"
- 数据直接写文件或上报 Church

Illuminator 的 Always-On 方案本质上是**对齐 perf_ebpf 的后端行为**，同时保留了 Web UI 的实时查看能力。
