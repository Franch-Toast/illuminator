import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useMemoryUtilization, useMemoryProcesses } from './useMemoryData'
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

describe('useMemoryUtilization', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and parse memory data', () => {
    const mockResponse = {
      pipeline: 'memory_utilization',
      records: [
        {
          labels: { type: 'memory_total' },
          fields: { total_mb: 16384, used_mb: 8192, cached_mb: 4096, buffers_mb: 512, free_mb: 3584 },
        },
        {
          labels: { type: 'swap' },
          fields: { used_mb: 256, total_mb: 2048 },
        },
        {
          labels: { type: 'vm_stats' },
          fields: { page_faults_per_sec: 150 },
        },
      ],
    }

    const { result } = renderHook(() => useMemoryUtilization(true))

    act(() => {
      mockBus.__emit('memory_utilization', mockResponse)
    })

    expect(result.current.summary).toBeTruthy()
    expect(result.current.summary!.totalMb).toBe(16384)
    expect(result.current.summary!.usedPct).toBeCloseTo(50)
    expect(result.current.summary!.swapUsedPct).toBeCloseTo(12.5)
    expect(result.current.summary!.pageFaults).toBe(150)

    const point = result.current.data[0]
    expect(point.used_mb).toBe(8192)
    expect(point.cached_mb).toBe(4096)
    expect(point.free_mb).toBe(3584)
  })

  it('should not poll when inactive', () => {
    renderHook(() => useMemoryUtilization(false))
    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })

  it('should clear data on clear()', () => {
    const { result } = renderHook(() => useMemoryUtilization(true))

    act(() => {
      mockBus.__emit('memory_utilization', {
        pipeline: 'memory_utilization',
        records: [{ labels: { type: 'memory_total' }, fields: { total_mb: 100, used_mb: 50, cached_mb: 20, buffers_mb: 5, free_mb: 25 } }],
      })
    })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })
})

describe('useMemoryProcesses', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and sort processes by RSS', () => {
    const mockResponse = {
      pipeline: 'memory_processes',
      records: [
        { labels: { type: 'process', pid: '100', comm: 'small' }, fields: { rss_mb: 50, vms_mb: 200, shared_mb: 10, swap_mb: 0, pss_mb: 45 } },
        { labels: { type: 'process', pid: '200', comm: 'big' }, fields: { rss_mb: 1024, vms_mb: 4096, shared_mb: 256, swap_mb: 12, pss_mb: 800 } },
      ],
    }

    const { result } = renderHook(() => useMemoryProcesses(true))

    act(() => {
      mockBus.__emit('memory_processes', mockResponse)
    })

    expect(result.current.processes.length).toBe(2)
    expect(result.current.processes[0].comm).toBe('big')
    expect(result.current.processes[0].rss_mb).toBe(1024)
    expect(result.current.processes[1].comm).toBe('small')
  })
})
