import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useNetworkMonitor, useNetworkProcesses, formatBytes } from './useNetworkData'
import type { DataBatch } from '../services/dataSource'
import { dataBus } from '../services/dataBus'

vi.mock('../services/dataBus', () => {
  const subscribers = new Map<string, Set<(batch: DataBatch) => void>>()

  return {
    dataBus: {
      subscribe: vi.fn((feature: string, cb: (batch: DataBatch) => void) => {
        if (!subscribers.has(feature)) subscribers.set(feature, new Set())
        subscribers.get(feature)!.add(cb)
        return () => { subscribers.get(feature)!.delete(cb) }
      }),
      getLatest: vi.fn(() => null),
      getAvailableFeatures: vi.fn(() => []),
      destroy: vi.fn(),
      onConnectionChange: vi.fn(() => () => {}),
      getStatus: vi.fn(() => 'disconnected' as const),
      connect: vi.fn(),
      disconnect: vi.fn(),
      __emit: (feature: string, data: unknown, timestamp = Date.now()) => {
        const batch: DataBatch = { feature, timestamp, data }
        subscribers.get(feature)?.forEach(cb => cb(batch))
      },
      __subscribers: subscribers,
    },
  }
})

vi.mock('../stores/useTimeStore', () => ({
  useTimeStore: (selector: (s: { mode: string }) => string) => selector({ mode: 'live' }),
}))

type MockDataBus = typeof dataBus & {
  __emit: (feature: string, data: unknown, timestamp?: number) => void
  __subscribers: Map<string, Set<(batch: DataBatch) => void>>
}

const mockBus = dataBus as MockDataBus

describe('useNetworkMonitor', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and parse network data', () => {
    const { result } = renderHook(() => useNetworkMonitor(true))

    act(() => {
      mockBus.__emit('net_tracer', {
        pipeline: 'net_tracer',
        records: [
          { labels: { type: 'net_total' }, fields: { rx_bytes_per_sec: 1048576, tx_bytes_per_sec: 524288, rx_packets_per_sec: 800, tx_packets_per_sec: 400 } },
          { labels: { type: 'tcp_stats' }, fields: { active_connections: 42, retransmits_per_sec: 3 } },
        ],
      })
    })

    expect(result.current.data.length).toBe(1)
    expect(result.current.summary).toBeTruthy()
    expect(result.current.summary!.rxRate).toBe(1048576)
    expect(result.current.summary!.txRate).toBe(524288)
    expect(result.current.summary!.connections).toBe(42)
    expect(result.current.summary!.retransmits).toBe(3)

    const point = result.current.data[0]
    expect(point.rx_packets_per_sec).toBe(800)
    expect(point.tcp_connections).toBe(42)
  })

  it('should not poll when inactive', () => {
    renderHook(() => useNetworkMonitor(false))
    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })

  it('should clear data', () => {
    const { result } = renderHook(() => useNetworkMonitor(true))

    act(() => {
      mockBus.__emit('net_tracer', {
        pipeline: 'net_tracer',
        records: [
          { labels: { type: 'net_total' }, fields: { rx_bytes_per_sec: 100, tx_bytes_per_sec: 50, rx_packets_per_sec: 10, tx_packets_per_sec: 5 } },
        ],
      })
    })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })
})

describe('useNetworkProcesses', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and sort processes by total traffic', () => {
    const { result } = renderHook(() => useNetworkProcesses(true))

    act(() => {
      mockBus.__emit('net_tracer', {
        pipeline: 'net_tracer',
        records: [
          { labels: { type: 'process_net', pid: '100', comm: 'low-net' }, fields: { rx_mb: 1, tx_mb: 2, connections: 3 } },
          { labels: { type: 'process_net', pid: '200', comm: 'high-net' }, fields: { rx_mb: 100, tx_mb: 50, connections: 12 } },
        ],
      })
    })

    expect(result.current.processes.length).toBe(2)
    expect(result.current.processes[0].comm).toBe('high-net')
    expect(result.current.processes[0].rx_mb).toBe(100)
    expect(result.current.processes[1].comm).toBe('low-net')
  })
})

describe('formatBytes', () => {
  it('should format bytes correctly', () => {
    expect(formatBytes(500)).toBe('500 B/s')
    expect(formatBytes(1500)).toBe('1.5 KB/s')
    expect(formatBytes(2 * 1024 * 1024)).toBe('2.0 MB/s')
    expect(formatBytes(1.5 * 1024 * 1024 * 1024)).toBe('1.5 GB/s')
  })
})
