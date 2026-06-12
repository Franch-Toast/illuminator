import { useState, useRef, useCallback, useEffect } from 'react'
import { api } from '../services/apiClient'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'

export interface MemoryDataPoint {
  timestamp: number
  used_mb: number
  cached_mb: number
  buffers_mb: number
  free_mb: number
  swap_used_mb: number
  swap_total_mb: number
}

export interface MemorySummary {
  totalMb: number
  usedPct: number
  swapUsedPct: number
  pageFaults: number
}

export interface MemoryProcess {
  pid: number
  comm: string
  rss_mb: number
  vms_mb: number
  shared_mb: number
  swap_mb: number
  pss_mb: number
  history: number[]
}

interface MemoryCollectResponse {
  pipeline: string
  records: Array<{
    labels: Record<string, string>
    fields: Record<string, number | string>
  }>
}

export function useMemoryUtilization(active: boolean, intervalMs = 1000) {
  const [data, setData] = useState<MemoryDataPoint[]>([])
  const [summary, setSummary] = useState<MemorySummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<MemoryDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('memory_utilization') as MemoryCollectResponse
        if (cancelled || !resp?.records) return

        const now = Date.now()
        let totalMb = 0
        let usedMb = 0
        let cachedMb = 0
        let buffersMb = 0
        let freeMb = 0
        let swapUsedMb = 0
        let swapTotalMb = 0
        let pageFaults = 0

        for (const rec of resp.records) {
          const type = rec.labels?.type
          if (type === 'memory_total') {
            totalMb = (rec.fields?.total_mb as number) ?? 0
            usedMb = (rec.fields?.used_mb as number) ?? 0
            cachedMb = (rec.fields?.cached_mb as number) ?? 0
            buffersMb = (rec.fields?.buffers_mb as number) ?? 0
            freeMb = (rec.fields?.free_mb as number) ?? 0
          } else if (type === 'swap') {
            swapUsedMb = (rec.fields?.used_mb as number) ?? 0
            swapTotalMb = (rec.fields?.total_mb as number) ?? 0
          } else if (type === 'vm_stats') {
            pageFaults = (rec.fields?.page_faults_per_sec as number) ?? 0
          }
        }

        const point: MemoryDataPoint = {
          timestamp: now,
          used_mb: usedMb,
          cached_mb: cachedMb,
          buffers_mb: buffersMb,
          free_mb: freeMb,
          swap_used_mb: swapUsedMb,
          swap_total_mb: swapTotalMb,
        }
        buffer.current.push(point)
        setData([...buffer.current.getAll()])

        setSummary({
          totalMb,
          usedPct: totalMb > 0 ? (usedMb / totalMb) * 100 : 0,
          swapUsedPct: swapTotalMb > 0 ? (swapUsedMb / swapTotalMb) * 100 : 0,
          pageFaults,
        })
      } catch {
        // retry next interval
      }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs, mode])

  const clear = useCallback(() => {
    buffer.current.clear()
    setData([])
    setSummary(null)
  }, [])

  return { data, summary, clear }
}

export function useMemoryProcesses(active: boolean, intervalMs = 2000) {
  const [processes, setProcesses] = useState<MemoryProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('memory_processes') as MemoryCollectResponse
        if (cancelled || !resp?.records) return

        const result: MemoryProcess[] = []
        for (const rec of resp.records) {
          if (rec.labels?.type !== 'process') continue
          const pid = parseInt(rec.labels?.pid ?? '0', 10)
          const rssMb = (rec.fields?.rss_mb as number) ?? 0

          const hist = historyMap.current.get(pid) ?? []
          hist.push(rssMb)
          if (hist.length > 30) hist.shift()
          historyMap.current.set(pid, hist)

          result.push({
            pid,
            comm: (rec.labels?.comm as string) ?? '?',
            rss_mb: rssMb,
            vms_mb: (rec.fields?.vms_mb as number) ?? 0,
            shared_mb: (rec.fields?.shared_mb as number) ?? 0,
            swap_mb: (rec.fields?.swap_mb as number) ?? 0,
            pss_mb: (rec.fields?.pss_mb as number) ?? 0,
            history: [...hist],
          })
        }

        result.sort((a, b) => b.rss_mb - a.rss_mb)
        setProcesses(result)
      } catch {
        // retry
      }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [active, intervalMs, mode])

  const clear = useCallback(() => {
    historyMap.current.clear()
    setProcesses([])
  }, [])

  return { processes, clear }
}
