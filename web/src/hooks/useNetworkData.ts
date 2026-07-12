import { useState, useRef, useCallback, useEffect } from 'react'
import { TimeSeriesBuffer } from './useFeatureStream'
import { useTimeStore } from '../stores/useTimeStore'
import { useDataSource } from './useDataSource'
import type { DataBatch, DataSource } from '../services/dataSource'
import { extractRecords } from '../utils/ssePayload'

export interface NetworkDataPoint {
  timestamp: number
  rx_bytes_per_sec: number
  tx_bytes_per_sec: number
  rx_packets_per_sec: number
  tx_packets_per_sec: number
  tcp_connections: number
  retransmits_per_sec: number
}

export interface NetworkSummary {
  rxRate: number
  txRate: number
  connections: number
  retransmits: number
}

export interface NetworkProcess {
  pid: number
  comm: string
  rx_mb: number
  tx_mb: number
  connections: number
  history: number[]
}

function transformNetworkMonitor(batch: DataBatch): { point: NetworkDataPoint; summary: NetworkSummary } | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const now = batch.timestamp
  let rxBytes = 0, txBytes = 0, rxPackets = 0, txPackets = 0
  let connections = 0, retransmits = 0

  for (const rec of records) {
    const type = rec.labels?.type
    if (type === 'net_total') {
      rxBytes = (rec.fields?.rx_bytes_per_sec as number) ?? 0
      txBytes = (rec.fields?.tx_bytes_per_sec as number) ?? 0
      rxPackets = (rec.fields?.rx_packets_per_sec as number) ?? 0
      txPackets = (rec.fields?.tx_packets_per_sec as number) ?? 0
    } else if (type === 'tcp_stats') {
      connections = (rec.fields?.active_connections as number) ?? 0
      retransmits = (rec.fields?.retransmits_per_sec as number) ?? 0
    }
  }

  const point: NetworkDataPoint = {
    timestamp: now, rx_bytes_per_sec: rxBytes, tx_bytes_per_sec: txBytes,
    rx_packets_per_sec: rxPackets, tx_packets_per_sec: txPackets,
    tcp_connections: connections, retransmits_per_sec: retransmits,
  }
  const summary: NetworkSummary = { rxRate: rxBytes, txRate: txBytes, connections, retransmits }
  return { point, summary }
}

export function useNetworkMonitor(active = true, replaySource?: DataSource) {
  const [data, setData] = useState<NetworkDataPoint[]>([])
  const [summary, setSummary] = useState<NetworkSummary | null>(null)
  const buffer = useRef(new TimeSeriesBuffer<NetworkDataPoint>(60))
  const mode = useTimeStore(s => s.mode)

  const { latest } = useDataSource({
    feature: 'net_tracer',
    transform: transformNetworkMonitor,
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

function transformNetworkProcesses(batch: DataBatch, historyMap: Map<number, number[]>): NetworkProcess[] | null {
  const records = extractRecords(batch.data)
  if (records.length === 0) return null

  const result: NetworkProcess[] = []
  for (const rec of records) {
    if (rec.labels?.type !== 'process_net') continue
    const pid = parseInt(rec.labels?.pid ?? '0', 10)
    const total = ((rec.fields?.rx_mb as number) ?? 0) + ((rec.fields?.tx_mb as number) ?? 0)

    const hist = historyMap.get(pid) ?? []
    hist.push(total)
    if (hist.length > 30) hist.shift()
    historyMap.set(pid, hist)

    result.push({
      pid, comm: (rec.labels?.comm as string) ?? '?',
      rx_mb: (rec.fields?.rx_mb as number) ?? 0, tx_mb: (rec.fields?.tx_mb as number) ?? 0,
      connections: (rec.fields?.connections as number) ?? 0, history: [...hist],
    })
  }
  result.sort((a, b) => (b.rx_mb + b.tx_mb) - (a.rx_mb + a.tx_mb))
  return result
}

export function useNetworkProcesses(active = true, replaySource?: DataSource) {
  const [processes, setProcesses] = useState<NetworkProcess[]>([])
  const historyMap = useRef<Map<number, number[]>>(new Map())
  const mode = useTimeStore(s => s.mode)

  const historyMapRef = historyMap.current
  const { latest } = useDataSource({
    feature: 'net_tracer',
    transform: useCallback((batch: DataBatch) => transformNetworkProcesses(batch, historyMapRef), [historyMapRef]),
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
