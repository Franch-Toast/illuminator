import { useState, useRef, useCallback, useEffect } from 'react'
import { api } from '../services/apiClient'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import type { DataBatch, DataSource } from '../services/dataSource'
import type { CpuDataPoint } from '../components/charts/StackedAreaChart'
import type { CoreDataPoint } from '../components/charts/CoreHeatmap'
import type { ProcessEntry } from '../components/charts/ProcessTable'
import type { CpuSummary } from '../components/charts/SummaryCards'

interface CpuCollectResponse {
  pipeline: string
  records: Array<{
    labels: Record<string, string>
    fields: Record<string, number | string>
  }>
}

function parseCpuUtilization(resp: CpuCollectResponse, now: number) {
  let totalRec: Record<string, number> | null = null
  const cores: { name: string; busy_pct: number }[] = []
  let ctxSwitches = 0
  let runQueue = 0

  for (const rec of resp.records) {
    const type = rec.labels?.type
    if (type === 'cpu_total') {
      totalRec = rec.fields as Record<string, number>
    } else if (type === 'cpu_core') {
      cores.push({
        name: rec.labels?.cpu ?? '?',
        busy_pct: (rec.fields?.busy_pct as number) ?? 0,
      })
    } else if (type === 'system_counters') {
      ctxSwitches = (rec.fields?.context_switches_per_sec as number) ?? 0
    } else if (type === 'runqueue') {
      runQueue = (rec.fields?.procs_running as number) ?? 0
    }
  }

  let point: CpuDataPoint | null = null
  if (totalRec) {
    point = {
      timestamp: now,
      user_pct: totalRec.user_pct ?? 0,
      system_pct: totalRec.system_pct ?? 0,
      irq_pct: totalRec.irq_pct ?? 0,
      softirq_pct: totalRec.softirq_pct ?? 0,
      iowait_pct: totalRec.iowait_pct ?? 0,
      steal_pct: totalRec.steal_pct ?? 0,
      idle_pct: totalRec.idle_pct ?? 0,
    }
  }

  const maxCore = cores.reduce(
    (acc, c) => c.busy_pct > acc.pct ? { name: c.name, pct: c.busy_pct } : acc,
    { name: '-', pct: 0 }
  )

  return {
    point,
    corePoint: cores.length > 0 ? { timestamp: now, cores } as CoreDataPoint : null,
    summary: {
      avgLoad: totalRec?.busy_pct ?? 0,
      maxCore,
      ctxSwitches,
      runQueue,
    } as CpuSummary,
  }
}

export function useCpuUtilization(active: boolean, intervalMs = 1000, replaySource?: DataSource) {
  const [areaData, setAreaData] = useState<CpuDataPoint[]>([])
  const [coreData, setCoreData] = useState<CoreDataPoint[]>([])
  const [summary, setSummary] = useState<CpuSummary | null>(null)
  const areaBuffer = useRef(new TimeSeriesBuffer<CpuDataPoint>(120))
  const coreBuffer = useRef(new TimeSeriesBuffer<CoreDataPoint>(120))
  const mode = useTimeStore(s => s.mode)

  const ingestBatch = useCallback((resp: CpuCollectResponse, ts: number) => {
    const parsed = parseCpuUtilization(resp, ts)
    if (parsed.point) {
      areaBuffer.current.push(parsed.point)
      setAreaData([...areaBuffer.current.getAll()])
    }
    if (parsed.corePoint) {
      coreBuffer.current.push(parsed.corePoint)
      setCoreData([...coreBuffer.current.getAll()])
    }
    setSummary(parsed.summary)
  }, [])

  useEffect(() => {
    if (replaySource) {
      const unsub = replaySource.subscribe('cpu_utilization', (batch: DataBatch) => {
        const data = batch.data as CpuCollectResponse
        if (data?.records) ingestBatch(data, batch.timestamp)
      })
      return unsub
    }

    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('cpu_utilization') as CpuCollectResponse
        if (cancelled || !resp?.records) return
        ingestBatch(resp, Date.now())
      } catch { /* retry next interval */ }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs, mode, replaySource, ingestBatch])

  const clear = useCallback(() => {
    areaBuffer.current.clear()
    coreBuffer.current.clear()
    setAreaData([])
    setCoreData([])
    setSummary(null)
  }, [])

  return { areaData, coreData, summary, clear }
}

function parseCpuProcesses(resp: CpuCollectResponse, historyMap: Map<number, number[]>): ProcessEntry[] {
  const result: ProcessEntry[] = []
  for (const rec of resp.records) {
    if (rec.labels?.type !== 'process') continue
    const pid = parseInt(rec.labels?.pid ?? '0', 10)
    const cpuTotal = (rec.fields?.cpu_total_pct as number) ?? 0

    const hist = historyMap.get(pid) ?? []
    hist.push(cpuTotal)
    if (hist.length > 30) hist.shift()
    historyMap.set(pid, hist)

    result.push({
      pid,
      comm: (rec.labels?.comm as string) ?? '?',
      cpu_total_pct: cpuTotal,
      cpu_user_pct: (rec.fields?.cpu_user_pct as number) ?? 0,
      cpu_sys_pct: (rec.fields?.cpu_sys_pct as number) ?? 0,
      num_threads: (rec.fields?.num_threads as number) ?? 0,
      rss_kb: (rec.fields?.rss_kb as number) ?? 0,
      state: (rec.fields?.state as string) ?? '?',
      history: [...hist],
    })
  }
  return result
}

export function useCpuProcesses(active: boolean, intervalMs = 2000, replaySource?: DataSource) {
  const [processes, setProcesses] = useState<ProcessEntry[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (replaySource) {
      const unsub = replaySource.subscribe('cpu_processes', (batch: DataBatch) => {
        const data = batch.data as CpuCollectResponse
        if (data?.records) {
          setProcesses(parseCpuProcesses(data, historyMap.current))
        }
      })
      return unsub
    }

    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('cpu_processes') as CpuCollectResponse
        if (cancelled || !resp?.records) return
        setProcesses(parseCpuProcesses(resp, historyMap.current))
      } catch { /* retry */ }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs, mode, replaySource])

  const clear = useCallback(() => {
    historyMap.current.clear()
    setProcesses([])
  }, [])

  return { processes, clear }
}
