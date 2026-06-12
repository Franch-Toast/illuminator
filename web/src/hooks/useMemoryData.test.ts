import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useMemoryUtilization, useMemoryProcesses } from './useMemoryData'

vi.mock('../services/apiClient', () => ({
  api: {
    featureCollect: vi.fn(),
  },
}))

vi.mock('../stores/useTimeStore', () => ({
  useTimeStore: (selector: (s: { mode: string }) => string) => selector({ mode: 'live' }),
}))

import { api } from '../services/apiClient'

describe('useMemoryUtilization', () => {
  beforeEach(() => {
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.clearAllMocks()
  })

  it('should fetch and parse memory data', async () => {
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

    vi.mocked(api.featureCollect).mockResolvedValue(mockResponse)

    const { result } = renderHook(() => useMemoryUtilization(true))

    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

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

  it('should not poll when inactive', async () => {
    renderHook(() => useMemoryUtilization(false))
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(api.featureCollect).not.toHaveBeenCalled()
  })

  it('should clear data on clear()', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'memory_utilization',
      records: [{ labels: { type: 'memory_total' }, fields: { total_mb: 100, used_mb: 50, cached_mb: 20, buffers_mb: 5, free_mb: 25 } }],
    })

    const { result } = renderHook(() => useMemoryUtilization(true))

    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })
})

describe('useMemoryProcesses', () => {
  beforeEach(() => {
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.clearAllMocks()
  })

  it('should fetch and sort processes by RSS', async () => {
    const mockResponse = {
      pipeline: 'memory_processes',
      records: [
        { labels: { type: 'process', pid: '100', comm: 'small' }, fields: { rss_mb: 50, vms_mb: 200, shared_mb: 10, swap_mb: 0, pss_mb: 45 } },
        { labels: { type: 'process', pid: '200', comm: 'big' }, fields: { rss_mb: 1024, vms_mb: 4096, shared_mb: 256, swap_mb: 12, pss_mb: 800 } },
      ],
    }

    vi.mocked(api.featureCollect).mockResolvedValue(mockResponse)

    const { result } = renderHook(() => useMemoryProcesses(true))

    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

    expect(result.current.processes.length).toBe(2)
    expect(result.current.processes[0].comm).toBe('big')
    expect(result.current.processes[0].rss_mb).toBe(1024)
    expect(result.current.processes[1].comm).toBe('small')
  })
})
