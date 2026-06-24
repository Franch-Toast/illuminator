import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { HttpLink } from './HttpLink'

vi.mock('../apiClient', () => ({
  api: {
    featureCollect: vi.fn().mockResolvedValue({ records: [{ ts: 1, cpu: 50 }] }),
  },
}))

describe('HttpLink', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks() })

  it('polls immediately on subscribe when active', async () => {
    const onData = vi.fn()
    const link = new HttpLink(onData, 1000)
    link.connect()
    link.subscribe('cpu_utilization')
    await vi.advanceTimersByTimeAsync(0)
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ feature: 'cpu_utilization' }))
  })

  it('continues polling at interval', async () => {
    const onData = vi.fn()
    const link = new HttpLink(onData, 500)
    link.connect()
    link.subscribe('memory_utilization')
    await vi.advanceTimersByTimeAsync(0)
    expect(onData).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(500)
    expect(onData).toHaveBeenCalledTimes(2)
  })

  it('stops polling on unsubscribe', async () => {
    const onData = vi.fn()
    const link = new HttpLink(onData, 500)
    link.connect()
    link.subscribe('io_throughput')
    await vi.advanceTimersByTimeAsync(0)
    link.unsubscribe('io_throughput')
    await vi.advanceTimersByTimeAsync(1000)
    expect(onData).toHaveBeenCalledTimes(1)
  })

  it('disconnect stops all timers', async () => {
    const onData = vi.fn()
    const link = new HttpLink(onData, 500)
    link.connect()
    link.subscribe('cpu_utilization')
    link.subscribe('memory_utilization')
    await vi.advanceTimersByTimeAsync(0)
    link.disconnect()
    await vi.advanceTimersByTimeAsync(2000)
    expect(onData).toHaveBeenCalledTimes(2) // only initial polls
  })

  it('isConnected reflects active state', () => {
    const link = new HttpLink(vi.fn(), 1000)
    expect(link.isConnected()).toBe(false)
    link.connect()
    expect(link.isConnected()).toBe(true)
    link.disconnect()
    expect(link.isConnected()).toBe(false)
  })
})
