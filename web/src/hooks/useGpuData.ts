import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { useDataSource } from './useDataSource'
import type { DataBatch, DataSource } from '../services/dataSource'
import { extractRecords } from '../utils/ssePayload'

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

function transformGpuMonitor(batch: DataBatch): { point: GpuDataPoint; summary: GpuSummary } | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const now = batch.timestamp
  let computePct = 0, memoryPct = 0, memUsedMb = 0, memTotalMb = 0
  let tempC = 0, powerW = 0

  for (const rec of records) {
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
    timestamp: now, compute_pct: computePct, memory_pct: memoryPct,
    memory_used_mb: memUsedMb, memory_total_mb: memTotalMb,
    temperature_c: tempC, power_w: powerW,
  }
  const summary: GpuSummary = {
    computePct, memoryUsedMb: memUsedMb, memoryTotalMb: memTotalMb, temperatureC: tempC, powerW: powerW,
  }
  return { point, summary }
}

export function useGpuMonitor(active = true, replaySource?: DataSource) {
  const [data, setData] = useState<GpuDataPoint[]>([])
  const [summary, setSummary] = useState<GpuSummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<GpuDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  const { latest } = useDataSource({
    feature: 'gpu_monitor',
    transform: transformGpuMonitor,
    active: active && mode !== 'paused',
    replaySource,
  })

  useEffect(() => {
    if (!latest) return
    buffer.current.push(latest.point)
    setData([...buffer.current.getAll()])
    setSummary(latest.summary)
  }, [latest])

  const clear = useCallback(() => {
    buffer.current.clear()
    setData([])
    setSummary(null)
  }, [])

  return { data, summary, clear }
}

function transformGpuProcesses(batch: DataBatch, historyMap: Map<number, number[]>): GpuProcess[] | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const result: GpuProcess[] = []
  for (const rec of records) {
    if (rec.labels?.type !== 'process_gpu') continue
    const pid = parseInt(rec.labels?.pid ?? '0', 10)
    const gpuPct = (rec.fields?.gpu_pct as number) ?? 0

    const hist = historyMap.get(pid) ?? []
    hist.push(gpuPct)
    if (hist.length > 30) hist.shift()
    historyMap.set(pid, hist)

    result.push({
      pid, comm: (rec.labels?.comm as string) ?? '?',
      gpu_pct: gpuPct, memory_mb: (rec.fields?.memory_mb as number) ?? 0,
      history: [...hist],
    })
  }
  result.sort((a, b) => b.gpu_pct - a.gpu_pct)
  return result
}

export function useGpuProcesses(active = true, replaySource?: DataSource) {
  const [processes, setProcesses] = useState<GpuProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  const historyMapRef = historyMap.current
  const { latest } = useDataSource({
    feature: 'gpu_monitor',
    transform: useCallback((batch: DataBatch) => transformGpuProcesses(batch, historyMapRef), [historyMapRef]),
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
