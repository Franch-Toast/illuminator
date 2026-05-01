import { useState, useEffect, useRef, useCallback } from 'react'

export interface CpuCoreMetrics {
  cpu: string
  type: string
  user_pct: number
  system_pct: number
  nice_pct: number
  idle_pct: number
  iowait_pct: number
  irq_pct: number
  softirq_pct: number
  steal_pct: number
  busy_pct: number
  user_pct_ema?: number
  system_pct_ema?: number
  busy_pct_ema?: number
}

export interface SystemCounters {
  context_switches_per_sec: number
  interrupts_per_sec: number
}

export interface LoadAvg {
  load_1m: number
  load_5m: number
  load_15m: number
}

export interface RunQueue {
  procs_running: number
  procs_blocked: number
}

export interface CpuUtilizationData {
  cores: CpuCoreMetrics[]
  total: CpuCoreMetrics | null
  counters: SystemCounters | null
  loadavg: LoadAvg | null
  runqueue: RunQueue | null
}

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

// Parse the /api/v1/cpu/utilization JSON response into structured data.
// The API returns { pipeline: "cpu_utilization", records: [...] }
// Each record has labels (source, type, cpu) and fields (user_pct, etc.)
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
        cpu,
        type,
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

const MAX_HISTORY = 900 // up to 15m at 1s refresh

export function useCpuUtilization(refreshMs = 1000) {
  const [data, setData] = useState<CpuUtilizationData>({
    cores: [],
    total: null,
    counters: null,
    loadavg: null,
    runqueue: null,
  })
  const [history, setHistory] = useState<Array<{ time: number } & Record<string, number>>>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | undefined>(undefined)

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/utilization')
      const json = (await res.json()) as unknown
      const parsed = parseUtilizationResponse(json)
      setData(parsed)
      setError(null)

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

export interface ProcessInfo {
  pid: string
  comm: string
  cpu_user_pct: number
  cpu_sys_pct: number
  cpu_total_pct: number
  state: string
  num_threads: number
  rss_kb: number
  vsize_kb: number
  voluntary_ctxt_switches: number
  nonvoluntary_ctxt_switches: number
  threads?: ThreadInfo[]
}

export interface ThreadInfo {
  pid: string
  tid: string
  comm: string
  parent_comm: string
  cpu_user_pct: number
  cpu_sys_pct: number
  cpu_total_pct: number
  state: string
}

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
        pid: labels.pid || '',
        comm: labels.comm || '',
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
        pid: labels.pid || '',
        tid: labels.tid || '',
        comm: labels.comm || '',
        parent_comm: labels.parent_comm || '',
        cpu_user_pct: Number(fields.cpu_user_pct) || 0,
        cpu_sys_pct: Number(fields.cpu_sys_pct) || 0,
        cpu_total_pct: Number(fields.cpu_total_pct) || 0,
        state: typeof fields.state === 'string' ? fields.state : '',
      })
    }
  }

  for (const t of threads) {
    const proc = processMap.get(t.pid)
    if (proc) proc.threads!.push(t)
  }

  return Array.from(processMap.values())
}

export function useProcessCpu(refreshMs = 2000) {
  const [processes, setProcesses] = useState<ProcessInfo[]>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval> | undefined>(undefined)

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/processes')
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
