import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { getDataSource } from './useDataSource'
import type { DataBatch } from '../services/dataSource'

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

export function useMemoryUtilization(active: boolean) {
  const [data, setData] = useState<MemoryDataPoint[]>([])
  const [summary, setSummary] = useState<MemorySummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<MemoryDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    const source = getDataSource()
    const unsub = source.subscribe('memory_utilization', (batch: DataBatch) => {
      const resp = batch.data as MemoryCollectResponse
      if (!resp?.records) return

      const now = batch.timestamp
      let totalMb = 0, usedMb = 0, cachedMb = 0, buffersMb = 0, freeMb = 0
      let swapUsedMb = 0, swapTotalMb = 0, pageFaults = 0

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
        used_mb: usedMb, cached_mb: cachedMb, buffers_mb: buffersMb,
        free_mb: freeMb, swap_used_mb: swapUsedMb, swap_total_mb: swapTotalMb,
      }
      buffer.current.push(point)
      setData([...buffer.current.getAll()])
      setSummary({
        totalMb,
        usedPct: totalMb > 0 ? (usedMb / totalMb) * 100 : 0,
        swapUsedPct: swapTotalMb > 0 ? (swapUsedMb / swapTotalMb) * 100 : 0,
        pageFaults,
      })
    })
    return unsub
  }, [active, mode])

  const clear = useCallback(() => {
    buffer.current.clear()
    setData([])
    setSummary(null)
  }, [])

  return { data, summary, clear }
}

export function useMemoryProcesses(active: boolean) {
  const [processes, setProcesses] = useState<MemoryProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    const source = getDataSource()
    const unsub = source.subscribe('memory_processes', (batch: DataBatch) => {
      const resp = batch.data as MemoryCollectResponse
      if (!resp?.records) return

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
    })
    return unsub
  }, [active, mode])

  const clear = useCallback(() => {
    historyMap.current.clear()
    setProcesses([])
  }, [])

  return { processes, clear }
}
