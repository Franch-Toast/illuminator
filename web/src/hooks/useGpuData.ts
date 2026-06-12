import { useState, useRef, useCallback, useEffect } from 'react'
import { api } from '../services/apiClient'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'

export interface GpuDataPoint {
  timestamp: number
  compute_pct: number
  memory_pct: number
  memory_used_mb: number
  memory_total_mb: number
  temperature_c: number
  power_w: number
}

export interface GpuSummary {
  computePct: number
  memoryUsedMb: number
  memoryTotalMb: number
  temperatureC: number
  powerW: number
}

export interface GpuProcess {
  pid: number
  comm: string
  gpu_pct: number
  memory_mb: number
  history: number[]
}

interface GpuCollectResponse {
  pipeline: string
  records: Array<{
    labels: Record<string, string>
    fields: Record<string, number | string>
  }>
}

export function useGpuMonitor(active: boolean, intervalMs = 1000) {
  const [data, setData] = useState<GpuDataPoint[]>([])
  const [summary, setSummary] = useState<GpuSummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<GpuDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('gpu_monitor') as GpuCollectResponse
        if (cancelled || !resp?.records) return

        const now = Date.now()
        let computePct = 0, memoryPct = 0
        let memUsedMb = 0, memTotalMb = 0
        let tempC = 0, powerW = 0

        for (const rec of resp.records) {
          const type = rec.labels?.type
          if (type === 'gpu_utilization') {
            computePct = (rec.fields?.compute_pct as number) ?? 0
            memoryPct = (rec.fields?.memory_pct as number) ?? 0
          } else if (type === 'gpu_memory') {
            memUsedMb = (rec.fields?.used_mb as number) ?? 0
            memTotalMb = (rec.fields?.total_mb as number) ?? 0
          } else if (type === 'gpu_thermal') {
            tempC = (rec.fields?.temperature_c as number) ?? 0
            powerW = (rec.fields?.power_w as number) ?? 0
          }
        }

        const point: GpuDataPoint = {
          timestamp: now,
          compute_pct: computePct,
          memory_pct: memoryPct,
          memory_used_mb: memUsedMb,
          memory_total_mb: memTotalMb,
          temperature_c: tempC,
          power_w: powerW,
        }
        buffer.current.push(point)
        setData([...buffer.current.getAll()])

        setSummary({
          computePct,
          memoryUsedMb: memUsedMb,
          memoryTotalMb: memTotalMb,
          temperatureC: tempC,
          powerW: powerW,
        })
      } catch {
        // retry
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

export function useGpuProcesses(active: boolean, intervalMs = 2000) {
  const [processes, setProcesses] = useState<GpuProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('gpu_monitor') as GpuCollectResponse
        if (cancelled || !resp?.records) return

        const result: GpuProcess[] = []
        for (const rec of resp.records) {
          if (rec.labels?.type !== 'process_gpu') continue
          const pid = parseInt(rec.labels?.pid ?? '0', 10)
          const gpuPct = (rec.fields?.gpu_pct as number) ?? 0

          const hist = historyMap.current.get(pid) ?? []
          hist.push(gpuPct)
          if (hist.length > 30) hist.shift()
          historyMap.current.set(pid, hist)

          result.push({
            pid,
            comm: (rec.labels?.comm as string) ?? '?',
            gpu_pct: gpuPct,
            memory_mb: (rec.fields?.memory_mb as number) ?? 0,
            history: [...hist],
          })
        }

        result.sort((a, b) => b.gpu_pct - a.gpu_pct)
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
