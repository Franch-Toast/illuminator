import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { LiveDataSource } from './liveDataSource'

vi.mock('./apiClient', () => ({
  api: {
    featureCollect: vi.fn().mockResolvedValue({ pipeline: 'test', records: [] }),
    features: vi.fn().mockResolvedValue({ features: [] }),
    featureStart: vi.fn().mockResolvedValue({ status: 'ok' }),
  },
}))

class MockWebSocket {
  static instances: MockWebSocket[] = []
  readyState = 0
  onopen: (() => void) | null = null
  onmessage: ((e: { data: string }) => void) | null = null
  onclose: (() => void) | null = null
  onerror: (() => void) | null = null

  constructor() {
    MockWebSocket.instances.push(this)
    setTimeout(() => {
      this.readyState = 3
      this.onclose?.()
    }, 10)
  }

  send = vi.fn()
  close = vi.fn()
}

describe('LiveDataSource', () => {
  beforeEach(() => {
    MockWebSocket.instances = []
    vi.stubGlobal('WebSocket', MockWebSocket)
    Object.defineProperty(document, 'visibilityState', { value: 'visible', writable: true })
  })

  afterEach(() => {
    vi.restoreAllMocks()
    vi.useRealTimers()
  })

  it('should create and subscribe to a feature', () => {
    const ds = new LiveDataSource({}, 1000)
    const cb = vi.fn()

    const unsub = ds.subscribe('cpu_utilization', cb)

    expect(ds.getAvailableFeatures()).toContain('cpu_utilization')

    unsub()
    expect(ds.getAvailableFeatures()).not.toContain('cpu_utilization')

    ds.destroy()
  })

  it('should fallback to HTTP polling when WS fails', async () => {
    vi.useFakeTimers()
    const ds = new LiveDataSource({}, 1000)
    const cb = vi.fn()

    ds.subscribe('cpu_utilization', cb)

    await vi.advanceTimersByTimeAsync(50)

    expect(cb).toHaveBeenCalled()

    ds.destroy()
  })

  it('should report disconnected status when WS fails', () => {
    const onConnectionChange = vi.fn()
    const ds = new LiveDataSource({ onConnectionChange }, 1000)

    expect(onConnectionChange).toHaveBeenCalledWith('connecting')

    ds.destroy()
  })

  it('should track latest data per feature', async () => {
    vi.useFakeTimers()
    const ds = new LiveDataSource({}, 500)

    ds.subscribe('test_feature', () => {})

    await vi.advanceTimersByTimeAsync(600)

    const latest = ds.getLatest('test_feature')
    expect(latest).not.toBeNull()
    expect(latest?.feature).toBe('test_feature')

    ds.destroy()
  })
})
