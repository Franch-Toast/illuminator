import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useIoMonitor, useIoProcesses } from './useIoData'

vi.mock('../services/apiClient', () => ({
  api: { featureCollect: vi.fn() },
}))

vi.mock('../stores/useTimeStore', () => ({
  useTimeStore: (selector: (s: { mode: string }) => string) => selector({ mode: 'live' }),
}))

import { api } from '../services/apiClient'

describe('useIoMonitor', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.clearAllMocks() })

  it('should fetch and parse IO data', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'io_monitor',
      records: [
        { labels: { type: 'io_total' }, fields: { read_iops: 500, write_iops: 200, read_throughput_mb: 50, write_throughput_mb: 20 } },
        { labels: { type: 'io_latency' }, fields: { avg_latency_us: 1200, p99_latency_us: 8500 } },
      ],
    })

    const { result } = renderHook(() => useIoMonitor(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

    expect(result.current.data.length).toBe(1)
    expect(result.current.summary).toBeTruthy()
    expect(result.current.summary!.totalIops).toBe(700)
    expect(result.current.summary!.readThroughput).toBe(50)
    expect(result.current.summary!.writeThroughput).toBe(20)
    expect(result.current.summary!.avgLatencyUs).toBe(1200)

    const point = result.current.data[0]
    expect(point.read_iops).toBe(500)
    expect(point.write_iops).toBe(200)
    expect(point.p99_latency_us).toBe(8500)
  })

  it('should not poll when inactive', async () => {
    renderHook(() => useIoMonitor(false))
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(api.featureCollect).not.toHaveBeenCalled()
  })

  it('should clear data', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'io_monitor',
      records: [
        { labels: { type: 'io_total' }, fields: { read_iops: 100, write_iops: 50, read_throughput_mb: 10, write_throughput_mb: 5 } },
      ],
    })

    const { result } = renderHook(() => useIoMonitor(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })
})

describe('useIoProcesses', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.clearAllMocks() })

  it('should fetch and sort processes by IOPS', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'io_monitor',
      records: [
        { labels: { type: 'process_io', pid: '100', comm: 'slow-io' }, fields: { read_mb: 1, write_mb: 2, iops: 50 } },
        { labels: { type: 'process_io', pid: '200', comm: 'fast-io' }, fields: { read_mb: 10, write_mb: 20, iops: 500 } },
      ],
    })

    const { result } = renderHook(() => useIoProcesses(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

    expect(result.current.processes.length).toBe(2)
    expect(result.current.processes[0].comm).toBe('fast-io')
    expect(result.current.processes[0].iops).toBe(500)
    expect(result.current.processes[1].comm).toBe('slow-io')
  })
})
