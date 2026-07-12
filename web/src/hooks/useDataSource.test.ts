import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useDataSource } from './useDataSource'
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

type MockDataBus = typeof dataBus & {
  __emit: (feature: string, data: unknown, timestamp?: number) => void
  __subscribers: Map<string, Set<(batch: DataBatch) => void>>
}

const mockBus = dataBus as MockDataBus

describe('useDataSource', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('returns loading initially and then data after first batch', () => {
    const { result } = renderHook(() =>
      useDataSource({
        feature: 'cpu_utilization',
        transform: (batch: DataBatch) => (batch.data as { value: number }).value,
      })
    )

    expect(result.current.loading).toBe(true)
    expect(result.current.data).toEqual([])

    act(() => {
      mockBus.__emit('cpu_utilization', { value: 42 })
    })

    expect(result.current.loading).toBe(false)
    expect(result.current.latest).toBe(42)
    expect(result.current.data).toEqual([42])
  })

  it('buffers data up to bufferSize', () => {
    const { result } = renderHook(() =>
      useDataSource({
        feature: 'cpu_utilization',
        transform: (batch: DataBatch) => (batch.data as { value: number }).value,
        bufferSize: 3,
      })
    )

    act(() => { mockBus.__emit('cpu_utilization', { value: 1 }) })
    act(() => { mockBus.__emit('cpu_utilization', { value: 2 }) })
    act(() => { mockBus.__emit('cpu_utilization', { value: 3 }) })
    act(() => { mockBus.__emit('cpu_utilization', { value: 4 }) })

    expect(result.current.data).toEqual([2, 3, 4])
  })

  it('sets error when transform throws', () => {
    const { result } = renderHook(() =>
      useDataSource({
        feature: 'cpu_utilization',
        transform: () => { throw new Error('parse error') },
      })
    )

    act(() => {
      mockBus.__emit('cpu_utilization', { value: 42 })
    })

    expect(result.current.error).not.toBeNull()
    expect(result.current.error!.message).toBe('parse error')
    expect(result.current.loading).toBe(false)
  })

  it('does not subscribe when inactive', () => {
    renderHook(() =>
      useDataSource({
        feature: 'cpu_utilization',
        transform: (batch: DataBatch) => (batch.data as { value: number }).value,
        active: false,
      })
    )

    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })
})
