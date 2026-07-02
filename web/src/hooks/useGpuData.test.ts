import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useGpuMonitor, useGpuProcesses } from './useGpuData'
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

describe('useGpuMonitor', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and parse GPU data from multiple record types', () => {
    const { result } = renderHook(() => useGpuMonitor(true))

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 75.5, memory_pct: 60.2 } },
          { labels: { type: 'gpu_memory' }, fields: { used_mb: 4096, total_mb: 8192 } },
          { labels: { type: 'gpu_thermal' }, fields: { temperature_c: 72, power_w: 180 } },
        ],
      })
    })

    expect(result.current.data.length).toBe(1)
    expect(result.current.summary).toBeTruthy()
    expect(result.current.summary!.computePct).toBe(75.5)
    expect(result.current.summary!.memoryUsedMb).toBe(4096)
    expect(result.current.summary!.memoryTotalMb).toBe(8192)
    expect(result.current.summary!.temperatureC).toBe(72)
    expect(result.current.summary!.powerW).toBe(180)

    const point = result.current.data[0]
    expect(point.compute_pct).toBe(75.5)
    expect(point.memory_pct).toBe(60.2)
    expect(point.memory_used_mb).toBe(4096)
  })

  it('should not poll when inactive', () => {
    renderHook(() => useGpuMonitor(false))
    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })

  it('should clear data', () => {
    const { result } = renderHook(() => useGpuMonitor(true))

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 50, memory_pct: 30 } },
        ],
      })
    })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })

  it('should accumulate data points over time', () => {
    const { result } = renderHook(() => useGpuMonitor(true))
    const t1 = Date.now()

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 10, memory_pct: 5 } },
        ],
      }, t1)
    })
    expect(result.current.data.length).toBe(1)

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 20, memory_pct: 10 } },
        ],
      }, t1 + 1000)
    })
    expect(result.current.data.length).toBe(2)
    expect(result.current.data[1].compute_pct).toBe(20)
  })
})

describe('useGpuProcesses', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and sort processes by GPU utilization', () => {
    const { result } = renderHook(() => useGpuProcesses(true))

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'light-gpu' }, fields: { gpu_pct: 5, memory_mb: 128 } },
          { labels: { type: 'process_gpu', pid: '200', comm: 'heavy-gpu' }, fields: { gpu_pct: 85, memory_mb: 2048 } },
          { labels: { type: 'process_gpu', pid: '300', comm: 'mid-gpu' }, fields: { gpu_pct: 30, memory_mb: 512 } },
        ],
      })
    })

    expect(result.current.processes.length).toBe(3)
    expect(result.current.processes[0].comm).toBe('heavy-gpu')
    expect(result.current.processes[0].gpu_pct).toBe(85)
    expect(result.current.processes[0].memory_mb).toBe(2048)
    expect(result.current.processes[1].comm).toBe('mid-gpu')
    expect(result.current.processes[2].comm).toBe('light-gpu')
  })

  it('should maintain history per process', () => {
    const { result } = renderHook(() => useGpuProcesses(true))
    const t1 = Date.now()

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: 20, memory_mb: 100 } },
        ],
      }, t1)
    })
    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: 30, memory_mb: 100 } },
        ],
      }, t1 + 1000)
    })
    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: 40, memory_mb: 100 } },
        ],
      }, t1 + 2000)
    })

    expect(result.current.processes[0].history.length).toBe(3)
    expect(result.current.processes[0].history).toEqual([20, 30, 40])
  })

  it('should clear processes and history', () => {
    const { result } = renderHook(() => useGpuProcesses(true))

    act(() => {
      mockBus.__emit('gpu_monitor', {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: 50, memory_mb: 256 } },
        ],
      })
    })
    expect(result.current.processes.length).toBe(1)

    act(() => { result.current.clear() })
    expect(result.current.processes).toHaveLength(0)
  })
})
