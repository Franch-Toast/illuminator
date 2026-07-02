import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useMetricsData, useMetricsHistory } from './useMetricsData'
import { dataBus } from '../services/dataBus'
import type { DataBatch, DataCallback } from '../services/dataSource'

vi.mock('../services/dataBus', () => {
  const subscribers = new Map<string, Set<DataCallback>>()

  return {
    dataBus: {
      subscribe: vi.fn((feature: string, cb: DataCallback) => {
        if (!subscribers.has(feature)) subscribers.set(feature, new Set())
        subscribers.get(feature)!.add(cb)
        return () => { subscribers.get(feature)!.delete(cb) }
      }),
      onConnectionChange: vi.fn(() => () => {}),
      connect: vi.fn(),
      disconnect: vi.fn(),
      __emit: (feature: string, data: unknown, modelType?: string) => {
        const batch: DataBatch = { feature, timestamp: Date.now(), modelType: modelType as DataBatch['modelType'], data }
        subscribers.get(feature)?.forEach(cb => cb(batch))
      },
      __subscribers: subscribers,
    },
  }
})

describe('useMetricsData', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    ;(dataBus as any).__subscribers.clear()
  })

  it('returns null initially', () => {
    const { result } = renderHook(() => useMetricsData('cpu_utilization'))
    expect(result.current).toBeNull()
  })

  it('subscribes to the correct feature', () => {
    renderHook(() => useMetricsData('cpu_utilization'))
    expect(dataBus.subscribe).toHaveBeenCalledWith('cpu_utilization', expect.any(Function))
  })

  it('updates state when data arrives', () => {
    const { result } = renderHook(() => useMetricsData('cpu_utilization'))

    act(() => {
      (dataBus as any).__emit('cpu_utilization', { value: 42 }, 'time_series')
    })

    expect(result.current).toEqual({ value: 42 })
  })

  it('ignores profile-type data', () => {
    const { result } = renderHook(() => useMetricsData('cpu_utilization'))

    act(() => {
      (dataBus as any).__emit('cpu_utilization', { value: 42 }, 'profile')
    })

    expect(result.current).toBeNull()
  })

  it('unsubscribes on unmount', () => {
    const { unmount } = renderHook(() => useMetricsData('cpu_utilization'))
    unmount()
    expect((dataBus as any).__subscribers.get('cpu_utilization')?.size ?? 0).toBe(0)
  })
})

describe('useMetricsHistory', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    ;(dataBus as any).__subscribers.clear()
  })

  it('returns empty array initially', () => {
    const { result } = renderHook(() => useMetricsHistory('cpu_utilization'))
    expect(result.current).toEqual([])
  })

  it('accumulates data', () => {
    const { result } = renderHook(() => useMetricsHistory('cpu_utilization'))

    act(() => {
      (dataBus as any).__emit('cpu_utilization', { value: 1 }, 'time_series')
      ;(dataBus as any).__emit('cpu_utilization', { value: 2 }, 'time_series')
      ;(dataBus as any).__emit('cpu_utilization', { value: 3 }, 'time_series')
    })

    expect(result.current).toHaveLength(3)
    expect(result.current[0]).toEqual({ value: 1 })
    expect(result.current[2]).toEqual({ value: 3 })
  })

  it('respects maxItems limit', () => {
    const { result } = renderHook(() => useMetricsHistory('cpu_utilization', 2))

    act(() => {
      (dataBus as any).__emit('cpu_utilization', { value: 1 }, 'time_series')
      ;(dataBus as any).__emit('cpu_utilization', { value: 2 }, 'time_series')
      ;(dataBus as any).__emit('cpu_utilization', { value: 3 }, 'time_series')
    })

    expect(result.current).toHaveLength(2)
    expect(result.current[0]).toEqual({ value: 2 })
    expect(result.current[1]).toEqual({ value: 3 })
  })
})
