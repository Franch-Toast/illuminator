import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderHook, act } from '@testing-library/react'
import { useCpuUtilization, useCpuProcesses } from './useCpuData'
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
  useTimeStore: (selector: (s: { mode: string }) => string) =>
    selector({ mode: 'live' }),
}))

const mockCpuUtilResponse = {
  pipeline: 'cpu_utilization',
  records: [
    { labels: { type: 'cpu_total' }, fields: { user_pct: 10, system_pct: 5, irq_pct: 0.5, softirq_pct: 0.2, iowait_pct: 1, steal_pct: 0, idle_pct: 83.3, busy_pct: 16.7 } },
    { labels: { type: 'cpu_core', cpu: 'cpu0' }, fields: { busy_pct: 25 } },
    { labels: { type: 'cpu_core', cpu: 'cpu1' }, fields: { busy_pct: 12 } },
    { labels: { type: 'system_counters' }, fields: { context_switches_per_sec: 8500 } },
    { labels: { type: 'runqueue' }, fields: { procs_running: 3 } },
  ],
}

const mockCpuProcessesResponse = {
  pipeline: 'cpu_processes',
  records: [
    { labels: { type: 'process', pid: '1234', comm: 'nginx' }, fields: { cpu_total_pct: 15.2, cpu_user_pct: 10.1, cpu_sys_pct: 5.1, num_threads: 8, rss_kb: 50000, state: 'S' } },
    { labels: { type: 'process', pid: '5678', comm: 'node' }, fields: { cpu_total_pct: 8.5, cpu_user_pct: 6.0, cpu_sys_pct: 2.5, num_threads: 12, rss_kb: 120000, state: 'S' } },
  ],
}

type MockDataBus = typeof dataBus & {
  __emit: (feature: string, data: unknown, timestamp?: number) => void
  __subscribers: Map<string, Set<(batch: DataBatch) => void>>
}

const mockBus = dataBus as MockDataBus

describe('useCpuUtilization', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and parse CPU utilization data', () => {
    const { result } = renderHook(() => useCpuUtilization(true))

    act(() => {
      mockBus.__emit('cpu_utilization', mockCpuUtilResponse)
    })

    expect(result.current.areaData.length).toBe(1)
    expect(result.current.areaData[0].user_pct).toBe(10)
    expect(result.current.areaData[0].system_pct).toBe(5)

    expect(result.current.coreData.length).toBe(1)
    expect(result.current.coreData[0].cores.length).toBe(2)

    expect(result.current.summary).not.toBeNull()
    expect(result.current.summary!.avgLoad).toBe(16.7)
    expect(result.current.summary!.ctxSwitches).toBe(8500)
    expect(result.current.summary!.runQueue).toBe(3)
    expect(result.current.summary!.maxCore.name).toBe('cpu0')
  })

  it('should accumulate data points over time', () => {
    const { result } = renderHook(() => useCpuUtilization(true))
    const t1 = Date.now()

    act(() => {
      mockBus.__emit('cpu_utilization', mockCpuUtilResponse, t1)
    })
    expect(result.current.areaData.length).toBe(1)

    act(() => {
      mockBus.__emit('cpu_utilization', mockCpuUtilResponse, t1 + 1000)
    })
    expect(result.current.areaData.length).toBe(2)
  })

  it('should not poll when inactive', () => {
    renderHook(() => useCpuUtilization(false))
    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })

  it('should clear data when clear() is called', () => {
    const { result } = renderHook(() => useCpuUtilization(true))

    act(() => {
      mockBus.__emit('cpu_utilization', mockCpuUtilResponse)
    })
    expect(result.current.areaData.length).toBe(1)

    act(() => { result.current.clear() })
    expect(result.current.areaData.length).toBe(0)
    expect(result.current.coreData.length).toBe(0)
    expect(result.current.summary).toBeNull()
  })

  it('should work with replaySource', async () => {
    vi.useFakeTimers()
    const mockSubscribe = vi.fn((_feature: string, cb: (batch: DataBatch) => void) => {
      setTimeout(() => {
        cb({ feature: 'cpu_utilization', timestamp: Date.now(), data: mockCpuUtilResponse })
      }, 50)
      return () => {}
    })
    const replaySource = { subscribe: mockSubscribe, getLatest: () => null, getAvailableFeatures: () => [], destroy: () => {} }

    const { result } = renderHook(() => useCpuUtilization(true, replaySource))

    await act(async () => { await vi.advanceTimersByTimeAsync(100) })

    expect(mockSubscribe).toHaveBeenCalledWith('cpu_utilization', expect.any(Function))
    expect(result.current.areaData.length).toBe(1)
    expect(dataBus.subscribe).not.toHaveBeenCalled()
    vi.useRealTimers()
  })
})

describe('useCpuProcesses', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    mockBus.__subscribers.clear()
  })

  it('should fetch and parse process data', () => {
    const { result } = renderHook(() => useCpuProcesses(true))

    act(() => {
      mockBus.__emit('process_cpu', mockCpuProcessesResponse)
    })

    expect(result.current.processes.length).toBe(2)
    expect(result.current.processes[0].pid).toBe(1234)
    expect(result.current.processes[0].comm).toBe('nginx')
    expect(result.current.processes[0].cpu_total_pct).toBe(15.2)
    expect(result.current.processes[0].num_threads).toBe(8)
  })

  it('should build history over multiple polls', () => {
    const { result } = renderHook(() => useCpuProcesses(true))
    const t1 = Date.now()

    act(() => {
      mockBus.__emit('process_cpu', mockCpuProcessesResponse, t1)
    })
    expect(result.current.processes[0].history!.length).toBe(1)

    act(() => {
      mockBus.__emit('process_cpu', mockCpuProcessesResponse, t1 + 1000)
    })
    expect(result.current.processes[0].history!.length).toBe(2)
  })

  it('should not poll when inactive', () => {
    renderHook(() => useCpuProcesses(false))
    expect(dataBus.subscribe).not.toHaveBeenCalled()
  })
})
