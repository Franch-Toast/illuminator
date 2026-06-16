import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { getDataSource } from './useDataSource'
import type { DataBatch } from '../services/dataSource'

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

interface IoCollectResponse {
  pipeline: string
  records: Array<{
    labels: Record<string, string>
    fields: Record<string, number | string>
  }>
}

export function useIoMonitor(active: boolean) {
  const [data, setData] = useState<IoDataPoint[]>([])
  const [summary, setSummary] = useState<IoSummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<IoDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    const source = getDataSource()
    const unsub = source.subscribe('io_monitor', (batch: DataBatch) => {
      const resp = batch.data as IoCollectResponse
      if (!resp?.records) return

      const now = batch.timestamp
      let readIops = 0, writeIops = 0
      let readThroughput = 0, writeThroughput = 0
      let avgLatency = 0, p99Latency = 0

      for (const rec of resp.records) {
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
      buffer.current.push(point)
      setData([...buffer.current.getAll()])
      setSummary({ totalIops: readIops + writeIops, readThroughput, writeThroughput, avgLatencyUs: avgLatency })
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

export function useIoProcesses(active: boolean) {
  const [processes, setProcesses] = useState<IoProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || mode === 'paused') return

    const source = getDataSource()
    const unsub = source.subscribe('io_monitor', (batch: DataBatch) => {
      const resp = batch.data as IoCollectResponse
      if (!resp?.records) return

      const result: IoProcess[] = []
      for (const rec of resp.records) {
        if (rec.labels?.type !== 'process_io') continue
        const pid = parseInt(rec.labels?.pid ?? '0', 10)
        const iops = (rec.fields?.iops as number) ?? 0

        const hist = historyMap.current.get(pid) ?? []
        hist.push(iops)
        if (hist.length > 30) hist.shift()
        historyMap.current.set(pid, hist)

        result.push({
          pid, comm: (rec.labels?.comm as string) ?? '?',
          read_mb: (rec.fields?.read_mb as number) ?? 0,
          write_mb: (rec.fields?.write_mb as number) ?? 0,
          iops, history: [...hist],
        })
      }
      result.sort((a, b) => b.iops - a.iops)
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
