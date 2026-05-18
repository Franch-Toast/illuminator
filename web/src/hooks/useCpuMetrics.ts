// ============================================================================
// Illuminator Web Hooks — CPU 指标数据获取和解析
// ============================================================================
//
// 本文件封装了 CPU 相关指标的前端数据获取逻辑，是连接到 C++ 后端
// /api/v1/cpu/* 端点的桥梁。
//
// 核心功能：
// ==========
// 1. useCpuUtilization(refreshMs)
//    - 定期 GET /api/v1/cpu/utilization 获取系统级 CPU 指标
//    - 解析 Record 格式（labels + fields）为结构化 TypeScript 类型
//    - 维护折线图所需的历史数据（最多保留 900 个数据点 ≈ 15 分钟）
//    - 自动轮询 + 组件卸载清理
//
// 2. useProcessCpu(refreshMs)
//    - 定期 GET /api/v1/cpu/processes 获取进程级 CPU 指标
//    - 将 Record 按 type 字段分拣为 process 和 thread 两种记录
//    - 自动将 thread 记录关联到所属的 process
//
// 响应格式示例：
// ==============
// {
//   "pipeline": "cpu_utilization",
//   "records": [
//     {
//       "labels": { "source": "cpu_utilization", "type": "cpu_total", "cpu": "cpu" },
//       "fields": { "user_pct": 12.5, "system_pct": 3.2, ... }
//     },
//     ...
//   ]
// }
//
// 注意：labels 在后端以数组 [{"key":"...","value":"..."}] 或对象形式传输，
// normalizeLabels() 对两种格式做了兼容处理。
// ============================================================================

import { useState, useEffect, useRef, useCallback } from 'react'

// ---- CPU 核心指标 ----
export interface CpuCoreMetrics {
  cpu: string           // CPU 标识（"cpu" 总计 / "cpu0","cpu1"... 单核）
  type: string          // 类型（"cpu_total" / "cpu_core"）
  user_pct: number      // 用户态 CPU 占用 %
  system_pct: number    // 内核态 CPU 占用 %
  nice_pct: number      // 低优先级用户态 %
  idle_pct: number      // 空闲 %
  iowait_pct: number    // IO 等待 %
  irq_pct: number       // 硬中断处理 %
  softirq_pct: number   // 软中断处理 %
  steal_pct: number     // 虚拟机偷取时间 %
  busy_pct: number      // 忙时间 %（total - idle - iowait）
  user_pct_ema?: number      // EMA 平滑后的用户态 %
  system_pct_ema?: number    // EMA 平滑后的内核态 %
  busy_pct_ema?: number      // EMA 平滑后的忙时间 %
}

// ---- 系统级计数器 ----
export interface SystemCounters {
  context_switches_per_sec: number   // 每秒上下文切换次数
  interrupts_per_sec: number         // 每秒中断次数
}

// ---- 负载均值 ----
export interface LoadAvg {
  load_1m: number   // 1 分钟平均负载
  load_5m: number   // 5 分钟平均负载
  load_15m: number  // 15 分钟平均负载
}

// ---- 运行队列统计 ----
export interface RunQueue {
  procs_running: number   // 可运行状态的进程数
  procs_blocked: number   // 阻塞状态的进程数
}

// ---- CPU 利用率完整数据 ----
export interface CpuUtilizationData {
  cores: CpuCoreMetrics[]        // 各核心的指标
  total: CpuCoreMetrics | null   // 总计（汇总所有核心）
  counters: SystemCounters | null
  loadavg: LoadAvg | null
  runqueue: RunQueue | null
}

// ---- 标签归一化 ----
// 后端的 labels 可能是数组 [{key, value}, ...] 或对象 {key: value, ...}
// 本函数统一转换为 Record<string,string> 方便后续读取
function normalizeLabels(raw: unknown): Record<string, string> {
  if (raw == null) return {}
  if (Array.isArray(raw)) {
    const out: Record<string, string> = {}
    for (const item of raw) {
      if (item && typeof item === 'object') {
        const o = item as Record<string, unknown>
        const key = o.key ?? o.name
        const val = o.value ?? o.val
        if (key != null) out[String(key)] = val != null ? String(val) : ''
      }
    }
    return out
  }
  if (typeof raw === 'object') {
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
      out[k] = v != null ? String(v) : ''
    }
    return out
  }
  return {}
}

// ---- 解析 CPU 利用率 API 响应 ----
// 按每个 record 的 type 标签分类填充 CpuUtilizationData
function parseUtilizationResponse(data: unknown): CpuUtilizationData {
  const result: CpuUtilizationData = {
    cores: [],
    total: null,
    counters: null,
    loadavg: null,
    runqueue: null,
  }
  const d = data as { records?: unknown[] } | null
  if (!d?.records) return result

  for (const rec of d.records) {
    const r = rec as { labels?: unknown; fields?: Record<string, number> }
    const labelObj = normalizeLabels(r.labels)
    const fields = r.fields || {}

    const type = labelObj.type || ''
    const cpu = labelObj.cpu || ''

    if (type === 'cpu_total' || type === 'cpu_core') {
      const core: CpuCoreMetrics = {
        cpu, type,
        user_pct: fields.user_pct ?? 0,
        system_pct: fields.system_pct ?? 0,
        nice_pct: fields.nice_pct ?? 0,
        idle_pct: fields.idle_pct ?? 0,
        iowait_pct: fields.iowait_pct ?? 0,
        irq_pct: fields.irq_pct ?? 0,
        softirq_pct: fields.softirq_pct ?? 0,
        steal_pct: fields.steal_pct ?? 0,
        busy_pct: fields.busy_pct ?? 0,
        user_pct_ema: fields.user_pct_ema,
        system_pct_ema: fields.system_pct_ema,
        busy_pct_ema: fields.busy_pct_ema,
      }
      if (type === 'cpu_total') result.total = core
      else result.cores.push(core)
    } else if (type === 'system_counters') {
      result.counters = {
        context_switches_per_sec: fields.context_switches_per_sec ?? 0,
        interrupts_per_sec: fields.interrupts_per_sec ?? 0,
      }
    } else if (type === 'loadavg') {
      result.loadavg = {
        load_1m: fields.load_1m ?? 0,
        load_5m: fields.load_5m ?? 0,
        load_15m: fields.load_15m ?? 0,
      }
    } else if (type === 'runqueue') {
      result.runqueue = {
        procs_running: fields.procs_running ?? 0,
        procs_blocked: fields.procs_blocked ?? 0,
      }
    }
  }
  return result
}

const MAX_HISTORY = 900 // 最多保留 900 个数据点（1 秒刷新 ≈ 15 分钟）

// ---- useCpuUtilization: 获取系统级 CPU 指标 ----
export function useCpuUtilization(refreshMs = 1000) {
  const [data, setData] = useState<CpuUtilizationData>({
    cores: [], total: null, counters: null, loadavg: null, runqueue: null,
  })
  const [history, setHistory] = useState<Array<{ time: number } & Record<string, number>>>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | undefined>(undefined)

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/utilization')
      if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`)
      const json = (await res.json()) as unknown
      const parsed = parseUtilizationResponse(json)
      setData(parsed)
      setError(null)

      // 将总计数据加入历史，供面积图使用
      if (parsed.total) {
        setHistory((prev) => {
          const entry = {
            time: Date.now(),
            user: parsed.total!.user_pct,
            system: parsed.total!.system_pct,
            iowait: parsed.total!.iowait_pct,
            irq: parsed.total!.irq_pct + parsed.total!.softirq_pct,
            steal: parsed.total!.steal_pct,
            idle: parsed.total!.idle_pct,
          }
          const next = [...prev, entry]
          return next.length > MAX_HISTORY ? next.slice(-MAX_HISTORY) : next
        })
      }
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    }
  }, [])

  useEffect(() => {
    fetchData()
    intervalRef.current = window.setInterval(fetchData, refreshMs)
    return () => {
      if (intervalRef.current !== undefined) clearInterval(intervalRef.current)
    }
  }, [fetchData, refreshMs])

  return { data, history, error }
}

// ---- 进程/线程信息（前端使用） ----
export interface ProcessInfo {
  pid: string                    // 进程 ID
  comm: string                   // 进程名称
  cpu_user_pct: number
  cpu_sys_pct: number
  cpu_total_pct: number
  state: string                  // 进程状态（R/S/D/Z/T...）
  num_threads: number
  rss_kb: number                 // 常驻内存 KB
  vsize_kb: number               // 虚拟内存 KB
  voluntary_ctxt_switches: number
  nonvoluntary_ctxt_switches: number
  threads?: ThreadInfo[]         // 关联的线程详情
}

export interface ThreadInfo {
  pid: string                    // 所属进程 ID
  tid: string                    // 线程 ID
  comm: string
  parent_comm: string            // 所属进程名
  cpu_user_pct: number
  cpu_sys_pct: number
  cpu_total_pct: number
  state: string
}

// ---- 解析进程 CPU 数据响应 ----
// 按 type 标签将 record 分为 process 和 thread，并建立关联
function parseProcessResponse(data: unknown): ProcessInfo[] {
  const d = data as { records?: unknown[] } | null
  if (!d?.records) return []
  const processMap = new Map<string, ProcessInfo>()
  const threads: ThreadInfo[] = []

  for (const rec of d.records) {
    const r = rec as { labels?: unknown; fields?: Record<string, unknown> }
    const labels = normalizeLabels(r.labels)
    const fields = r.fields || {}
    const type = labels.type || ''

    if (type === 'process') {
      processMap.set(labels.pid, {
        pid: labels.pid || '', comm: labels.comm || '',
        cpu_user_pct: Number(fields.cpu_user_pct) || 0,
        cpu_sys_pct: Number(fields.cpu_sys_pct) || 0,
        cpu_total_pct: Number(fields.cpu_total_pct) || 0,
        state: typeof fields.state === 'string' ? fields.state : '',
        num_threads: Number(fields.num_threads) || 0,
        rss_kb: Number(fields.rss_kb) || 0,
        vsize_kb: Number(fields.vsize_kb) || 0,
        voluntary_ctxt_switches: Number(fields.voluntary_ctxt_switches) || 0,
        nonvoluntary_ctxt_switches: Number(fields.nonvoluntary_ctxt_switches) || 0,
        threads: [],
      })
    } else if (type === 'thread') {
      threads.push({
        pid: labels.pid || '', tid: labels.tid || '',
        comm: labels.comm || '', parent_comm: labels.parent_comm || '',
        cpu_user_pct: Number(fields.cpu_user_pct) || 0,
        cpu_sys_pct: Number(fields.cpu_sys_pct) || 0,
        cpu_total_pct: Number(fields.cpu_total_pct) || 0,
        state: typeof fields.state === 'string' ? fields.state : '',
      })
    }
  }

  // 将线程关联到所属进程
  for (const t of threads) {
    const proc = processMap.get(t.pid)
    if (proc) proc.threads!.push(t)
  }

  return Array.from(processMap.values())
}

// ---- useProcessCpu: 获取进程级 CPU 指标 ----
export function useProcessCpu(refreshMs = 2000) {
  const [processes, setProcesses] = useState<ProcessInfo[]>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | undefined>(undefined)

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/processes')
      if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`)
      const json = (await res.json()) as unknown
      setProcesses(parseProcessResponse(json))
      setError(null)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    }
  }, [])

  useEffect(() => {
    fetchData()
    intervalRef.current = window.setInterval(fetchData, refreshMs)
    return () => {
      if (intervalRef.current !== undefined) clearInterval(intervalRef.current)
    }
  }, [fetchData, refreshMs])

  return { processes, error }
}
