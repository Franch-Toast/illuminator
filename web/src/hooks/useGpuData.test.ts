import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useGpuMonitor, useGpuProcesses } from './useGpuData'

vi.mock('../services/apiClient', () => ({
  api: { featureCollect: vi.fn() },
}))

vi.mock('../stores/useTimeStore', () => ({
  useTimeStore: (selector: (s: { mode: string }) => string) => selector({ mode: 'live' }),
}))

import { api } from '../services/apiClient'

describe('useGpuMonitor', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.clearAllMocks() })

  it('should fetch and parse GPU data from multiple record types', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'gpu_monitor',
      records: [
        { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 75.5, memory_pct: 60.2 } },
        { labels: { type: 'gpu_memory' }, fields: { used_mb: 4096, total_mb: 8192 } },
        { labels: { type: 'gpu_thermal' }, fields: { temperature_c: 72, power_w: 180 } },
      ],
    })

    const { result } = renderHook(() => useGpuMonitor(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

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

  it('should not poll when inactive', async () => {
    renderHook(() => useGpuMonitor(false))
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(api.featureCollect).not.toHaveBeenCalled()
  })

  it('should clear data', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'gpu_monitor',
      records: [
        { labels: { type: 'gpu_utilization' }, fields: { compute_pct: 50, memory_pct: 30 } },
      ],
    })

    const { result } = renderHook(() => useGpuMonitor(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    expect(result.current.data.length).toBeGreaterThan(0)

    act(() => { result.current.clear() })
    expect(result.current.data).toHaveLength(0)
    expect(result.current.summary).toBeNull()
  })

  it('should accumulate data points over time', async () => {
    let callCount = 0
    vi.mocked(api.featureCollect).mockImplementation(async () => {
      callCount++
      return {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'gpu_utilization' }, fields: { compute_pct: callCount * 10, memory_pct: callCount * 5 } },
        ],
      }
    })

    const { result } = renderHook(() => useGpuMonitor(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    expect(result.current.data.length).toBe(1)

    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
    expect(result.current.data.length).toBe(2)
    expect(result.current.data[1].compute_pct).toBe(20)
  })
})

describe('useGpuProcesses', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.clearAllMocks() })

  it('should fetch and sort processes by GPU utilization', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'gpu_monitor',
      records: [
        { labels: { type: 'process_gpu', pid: '100', comm: 'light-gpu' }, fields: { gpu_pct: 5, memory_mb: 128 } },
        { labels: { type: 'process_gpu', pid: '200', comm: 'heavy-gpu' }, fields: { gpu_pct: 85, memory_mb: 2048 } },
        { labels: { type: 'process_gpu', pid: '300', comm: 'mid-gpu' }, fields: { gpu_pct: 30, memory_mb: 512 } },
      ],
    })

    const { result } = renderHook(() => useGpuProcesses(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

    expect(result.current.processes.length).toBe(3)
    expect(result.current.processes[0].comm).toBe('heavy-gpu')
    expect(result.current.processes[0].gpu_pct).toBe(85)
    expect(result.current.processes[0].memory_mb).toBe(2048)
    expect(result.current.processes[1].comm).toBe('mid-gpu')
    expect(result.current.processes[2].comm).toBe('light-gpu')
  })

  it('should maintain history per process', async () => {
    let val = 10
    vi.mocked(api.featureCollect).mockImplementation(async () => {
      val += 10
      return {
        pipeline: 'gpu_monitor',
        records: [
          { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: val, memory_mb: 100 } },
        ],
      }
    })

    const { result } = renderHook(() => useGpuProcesses(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })

    expect(result.current.processes[0].history.length).toBe(3)
    expect(result.current.processes[0].history).toEqual([20, 30, 40])
  })

  it('should clear processes and history', async () => {
    vi.mocked(api.featureCollect).mockResolvedValue({
      pipeline: 'gpu_monitor',
      records: [
        { labels: { type: 'process_gpu', pid: '100', comm: 'test' }, fields: { gpu_pct: 50, memory_mb: 256 } },
      ],
    })

    const { result } = renderHook(() => useGpuProcesses(true))
    await act(async () => { await vi.advanceTimersByTimeAsync(100) })
    expect(result.current.processes.length).toBe(1)

    act(() => { result.current.clear() })
    expect(result.current.processes).toHaveLength(0)
  })
})
