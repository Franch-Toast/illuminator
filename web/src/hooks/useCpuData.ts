import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { useDataSource } from './useDataSource'
import type { DataBatch, DataSource } from '../services/dataSource'
import type { CpuDataPoint } from '../components/charts/StackedAreaChart'
import type { CoreDataPoint } from '../components/charts/CoreHeatmap'
import type { ProcessEntry } from '../components/charts/ProcessTable'
import type { CpuSummary } from '../components/charts/SummaryCards'

import { extractRecords, type SseRecord } from '../utils/ssePayload'

export interface CpuUtilizationPayload {
  point: CpuDataPoint | null
  corePoint: CoreDataPoint | null
  summary: CpuSummary
}

function parseCpuUtilization(records: SseRecord[], now: number): CpuUtilizationPayload {
  let totalRec: Record<string, number> | null = null
  const cores: { name: string; busy_pct: number }[] = []
  let ctxSwitches = 0
  let runQueue = 0

  for (const rec of records) {
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

function transformCpuUtilization(batch: DataBatch): CpuUtilizationPayload | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null
  return parseCpuUtilization(records, batch.timestamp)
}

export function useCpuUtilization(active = true, replaySource?: DataSource) {
  const [areaData, setAreaData] = useState<CpuDataPoint[]>([])
  const [coreData, setCoreData] = useState<CoreDataPoint[]>([])
  const [summary, setSummary] = useState<CpuSummary | null>(null)
  const areaBuffer = useRef(new TimeSeriesBuffer<CpuDataPoint>(120))
  const coreBuffer = useRef(new TimeSeriesBuffer<CoreDataPoint>(120))
  const mode = useTimeStore(s => s.mode)

  const { latest } = useDataSource({
    feature: 'cpu_utilization',
    transform: transformCpuUtilization,
    active: active && mode !== 'paused',
    replaySource,
  })

  useEffect(() => {
    if (!latest) return
    if (latest.point) {
      areaBuffer.current.push(latest.point)
      setAreaData([...areaBuffer.current.getAll()])
    }
    if (latest.corePoint) {
      coreBuffer.current.push(latest.corePoint)
      setCoreData([...coreBuffer.current.getAll()])
    }
    setSummary(latest.summary)
  }, [latest])

  const clear = useCallback(() => {
    areaBuffer.current.clear()
    coreBuffer.current.clear()
    setAreaData([])
    setCoreData([])
    setSummary(null)
  }, [])

  return { areaData, coreData, summary, clear }
}

function parseCpuProcesses(records: SseRecord[], historyMap: Map<number, number[]>): ProcessEntry[] {
  const result: ProcessEntry[] = []
  for (const rec of records) {
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

function transformCpuProcesses(batch: DataBatch, historyMap: Map<number, number[]>): ProcessEntry[] | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null
  return parseCpuProcesses(records, historyMap)
}

export function useCpuProcesses(active = true, replaySource?: DataSource) {
  const [processes, setProcesses] = useState<ProcessEntry[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  const historyMapRef = historyMap.current
  const { latest } = useDataSource({
    feature: 'process_cpu',
    transform: useCallback((batch: DataBatch) => transformCpuProcesses(batch, historyMapRef), [historyMapRef]),
    active: active && mode !== 'paused',
    replaySource,
  })

  useEffect(() => {
    if (!latest) return
    setProcesses(latest)
  }, [latest])

  const clear = useCallback(() => {
    historyMap.current.clear()
    setProcesses([])
  }, [])

  return { processes, clear }
}
