import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { useDataSource } from './useDataSource'
import type { DataBatch, DataSource } from '../services/dataSource'
import { extractRecords } from '../utils/ssePayload'

export interface IoDataPoint {
  timestamp: number
  read_iops: number
  write_iops: number
  read_throughput_mb: number
  write_throughput_mb: number
  avg_latency_us: number
  p99_latency_us: number
}

export interface IoSummary {
  totalIops: number
  readThroughput: number
  writeThroughput: number
  avgLatencyUs: number
}

export interface IoProcess {
  pid: number
  comm: string
  read_mb: number
  write_mb: number
  iops: number
  history: number[]
}

function transformIoMonitor(batch: DataBatch): { point: IoDataPoint; summary: IoSummary } | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const now = batch.timestamp
  let readIops = 0, writeIops = 0
  let readThroughput = 0, writeThroughput = 0
  let avgLatency = 0, p99Latency = 0

  for (const rec of records) {
    const type = rec.labels?.type
    if (type === 'io_total') {
      readIops = (rec.fields?.read_iops as number) ?? 0
      writeIops = (rec.fields?.write_iops as number) ?? 0
      readThroughput = (rec.fields?.read_throughput_mb as number) ?? 0
      writeThroughput = (rec.fields?.write_throughput_mb as number) ?? 0
    } else if (type === 'io_latency') {
      avgLatency = (rec.fields?.avg_latency_us as number) ?? 0
      p99Latency = (rec.fields?.p99_latency_us as number) ?? 0
    }
  }

  const point: IoDataPoint = {
    timestamp: now, read_iops: readIops, write_iops: writeIops,
    read_throughput_mb: readThroughput, write_throughput_mb: writeThroughput,
    avg_latency_us: avgLatency, p99_latency_us: p99Latency,
  }
  const summary: IoSummary = {
    totalIops: readIops + writeIops, readThroughput, writeThroughput, avgLatencyUs: avgLatency,
  }
  return { point, summary }
}

export function useIoMonitor(active = true, replaySource?: DataSource) {
  const [data, setData] = useState<IoDataPoint[]>([])
  const [summary, setSummary] = useState<IoSummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<IoDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  const { latest } = useDataSource({
    feature: 'io_monitor',
    transform: transformIoMonitor,
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

function transformIoProcesses(batch: DataBatch, historyMap: Map<number, number[]>): IoProcess[] | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const result: IoProcess[] = []
  for (const rec of records) {
    if (rec.labels?.type !== 'process_io') continue
    const pid = parseInt(rec.labels?.pid ?? '0', 10)
    const iops = (rec.fields?.iops as number) ?? 0

    const hist = historyMap.get(pid) ?? []
    hist.push(iops)
    if (hist.length > 30) hist.shift()
    historyMap.set(pid, hist)

    result.push({
      pid, comm: (rec.labels?.comm as string) ?? '?',
      read_mb: (rec.fields?.read_mb as number) ?? 0,
      write_mb: (rec.fields?.write_mb as number) ?? 0,
      iops, history: [...hist],
    })
  }
  result.sort((a, b) => b.iops - a.iops)
  return result
}

export function useIoProcesses(active = true, replaySource?: DataSource) {
  const [processes, setProcesses] = useState<IoProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  const historyMapRef = historyMap.current
  const { latest } = useDataSource({
    feature: 'io_monitor',
    transform: useCallback((batch: DataBatch) => transformIoProcesses(batch, historyMapRef), [historyMapRef]),
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

export function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB/s`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB/s`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB/s`
  return `${bytes.toFixed(0)} B/s`
}
