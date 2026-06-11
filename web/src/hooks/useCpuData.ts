import { useState, useRef, useCallback, useEffect } from 'react'
import { api } from '../services/apiClient'
import { TimeSeriesBuffer } from './useFeatureStream'
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

export function useCpuUtilization(active: boolean, intervalMs = 1000) {
  const [areaData, setAreaData] = useState<CpuDataPoint[]>([])
  const [coreData, setCoreData] = useState<CoreDataPoint[]>([])
  const [summary, setSummary] = useState<CpuSummary | null>(null)
  const areaBuffer = useRef(new TimeSeriesBuffer<CpuDataPoint>(60))
  const coreBuffer = useRef(new TimeSeriesBuffer<CoreDataPoint>(60))

  useEffect(() => {
    if (!active) return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('cpu_utilization') as CpuCollectResponse
        if (cancelled || !resp?.records) return

        const now = Date.now()
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

        if (totalRec) {
          const point: CpuDataPoint = {
            timestamp: now,
            user_pct: totalRec.user_pct ?? 0,
            system_pct: totalRec.system_pct ?? 0,
            irq_pct: totalRec.irq_pct ?? 0,
            softirq_pct: totalRec.softirq_pct ?? 0,
            iowait_pct: totalRec.iowait_pct ?? 0,
            steal_pct: totalRec.steal_pct ?? 0,
            idle_pct: totalRec.idle_pct ?? 0,
          }
          areaBuffer.current.push(point)
          setAreaData([...areaBuffer.current.getAll()])
        }

        if (cores.length > 0) {
          const cp: CoreDataPoint = { timestamp: now, cores }
          coreBuffer.current.push(cp)
          setCoreData([...coreBuffer.current.getAll()])
        }

        const maxCore = cores.reduce(
          (acc, c) => c.busy_pct > acc.pct ? { name: c.name, pct: c.busy_pct } : acc,
          { name: '-', pct: 0 }
        )
        setSummary({
          avgLoad: totalRec?.busy_pct ?? 0,
          maxCore,
          ctxSwitches,
          runQueue,
        })
      } catch {
        // retry next interval
      }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs])

  const clear = useCallback(() => {
    areaBuffer.current.clear()
    coreBuffer.current.clear()
    setAreaData([])
    setCoreData([])
    setSummary(null)
  }, [])

  return { areaData, coreData, summary, clear }
}

export function useCpuProcesses(active: boolean, intervalMs = 2000) {
  const [processes, setProcesses] = useState<ProcessEntry[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())

  useEffect(() => {
    if (!active) return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('cpu_processes') as CpuCollectResponse
        if (cancelled || !resp?.records) return

        const result: ProcessEntry[] = []
        for (const rec of resp.records) {
          if (rec.labels?.type !== 'process') continue
          const pid = parseInt(rec.labels?.pid ?? '0', 10)
          const cpuTotal = (rec.fields?.cpu_total_pct as number) ?? 0

          const hist = historyMap.current.get(pid) ?? []
          hist.push(cpuTotal)
          if (hist.length > 30) hist.shift()
          historyMap.current.set(pid, hist)

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
        setProcesses(result)
      } catch {
        // retry
      }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs])

  const clear = useCallback(() => {
    historyMap.current.clear()
    setProcesses([])
  }, [])

  return { processes, clear }
}
