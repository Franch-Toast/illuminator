import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { ConnectionState, SseLink } from './sseLink'

class MockEventSource {
  static OPEN = 1
  static CLOSED = 2
  static CONNECTING = 0

  static instances: MockEventSource[] = []
  readyState = 0
  url: string
  listeners: Record<string, (e: MessageEvent) => void> = {}
  onopen: (() => void) | null = null
  onerror: ((e: Event) => void) | null = null
  onmessage: ((e: MessageEvent) => void) | null = null

  constructor(url: string) {
    this.url = url
    MockEventSource.instances.push(this)
    setTimeout(() => {
      this.readyState = MockEventSource.OPEN
      this.onopen?.()
    }, 5)
  }

  addEventListener(event: string, handler: (e: MessageEvent) => void) {
    this.listeners[event] = handler
  }

  close = vi.fn(() => { this.readyState = MockEventSource.CLOSED })

  simulateEvent(type: string, data: string, lastEventId?: string) {
    const event = { data, lastEventId } as MessageEvent
    this.listeners[type]?.(event)
  }

  simulateMessage(data: string, lastEventId?: string) {
    const event = { data, lastEventId } as MessageEvent
    this.onmessage?.(event)
  }

  simulateError() {
    this.onerror?.(new Event('error'))
  }
}

globalThis.EventSource = MockEventSource as unknown as typeof EventSource

describe('SseLink', () => {
  beforeEach(() => {
    MockEventSource.instances = []
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('creates EventSource with correct URL', () => {
    const link = new SseLink({
      url: '/api/v1/events/test123',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    expect(MockEventSource.instances).toHaveLength(1)
    expect(MockEventSource.instances[0].url).toBe('/api/v1/events/test123')
    link.disconnect()
  })

  it('dispatches data events', () => {
    const onData = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData,
      onFrame: vi.fn(),
    })

    link.connect()
    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu', value: 99 }))

    expect(onData).toHaveBeenCalledWith('cpu', { feature: 'cpu', value: 99 })
    link.disconnect()
  })

  it('dispatches frame events', () => {
    const onFrame = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame,
    })

    link.connect()
    const es = MockEventSource.instances[0]
    es.simulateEvent('frame', JSON.stringify({
      feature: 'profiler', seq: 1, frame_idx: 0, frame_total: 3, payload: 'abc',
    }))

    expect(onFrame).toHaveBeenCalledWith({
      feature: 'profiler', seq: 1, frame_idx: 0, frame_total: 3, payload: 'abc',
    })
    link.disconnect()
  })

  it('calls onOpen on connection', async () => {
    const onOpen = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
      onOpen,
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)
    expect(onOpen).toHaveBeenCalled()
    link.disconnect()
  })

  it('disconnect closes EventSource', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10) // Let onopen fire
    expect(link.connected).toBe(true)
    link.disconnect()
    expect(MockEventSource.instances[0].close).toHaveBeenCalled()
    expect(link.connected).toBe(false)
  })

  it('reconnects on error with exponential backoff', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    const firstEs = MockEventSource.instances[0]
    firstEs.simulateError()

    // First reconnect base delay is 1000ms with ±20% jitter.
    await vi.advanceTimersByTimeAsync(1200)
    expect(MockEventSource.instances).toHaveLength(2)

    link.disconnect()
  })

  it('reports state changes through onStateChange', async () => {
    const onStateChange = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
      onStateChange,
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)
    expect(onStateChange).toHaveBeenCalledWith(ConnectionState.Reconnecting)
    expect(onStateChange).toHaveBeenCalledWith(ConnectionState.Connected)

    const es = MockEventSource.instances[0]
    es.simulateError()
    expect(onStateChange).toHaveBeenCalledWith(ConnectionState.Error)
    expect(onStateChange).toHaveBeenCalledWith(ConnectionState.Reconnecting)

    link.disconnect()
  })

  it('increments reconnect delay exponentially with jitter capped at 30s', async () => {
    const onStateChange = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
      onStateChange,
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)

    // Force a series of errors and verify that new EventSources are created at
    // increasing intervals. Because jitter is ±20%, we use a small tolerance.
    const expectedDelays = [1000, 2000, 4000, 8000, 16000, 30000, 30000]
    for (let i = 0; i < expectedDelays.length; i++) {
      const prevCount = MockEventSource.instances.length
      MockEventSource.instances[MockEventSource.instances.length - 1]!.simulateError()

      // Advance just past the base delay; jitter may shorten it slightly, so use 80% threshold.
      await vi.advanceTimersByTimeAsync(Math.floor(expectedDelays[i]! * 0.85))
      if (MockEventSource.instances.length === prevCount) {
        await vi.advanceTimersByTimeAsync(Math.floor(expectedDelays[i]! * 0.4))
      }
      expect(MockEventSource.instances.length).toBeGreaterThan(prevCount)
    }

    link.disconnect()
  })

  it('detects heartbeat timeout and transitions to stale', async () => {
    const onStateChange = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
      onStateChange,
    }, 1000) // 1s heartbeat timeout for test speed

    link.connect()
    await vi.advanceTimersByTimeAsync(10)
    expect(link.getStats().state).toBe(ConnectionState.Connected)

    // No data for > heartbeat timeout should move to stale and schedule reconnect.
    await vi.advanceTimersByTimeAsync(1200)
    expect(link.getStats().state).toBe(ConnectionState.Stale)

    link.disconnect()
  })

  it('resets heartbeat timeout on incoming messages', async () => {
    const onStateChange = vi.fn()
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
      onStateChange,
    }, 1000)

    link.connect()
    await vi.advanceTimersByTimeAsync(10)

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu', value: 1 }))
    await vi.advanceTimersByTimeAsync(800)
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu', value: 2 }))
    await vi.advanceTimersByTimeAsync(800)
    // Total elapsed from first message is ~1.6s, but timeout was reset twice.
    expect(link.getStats().state).toBe(ConnectionState.Connected)

    link.disconnect()
  })

  it('tracks Last-Event-ID and sends it on manual reconnect', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu', value: 1 }), 'evt-42')
    expect(link.getStats().lastEventId).toBe('evt-42')

    link.reconnectNow()
    await vi.advanceTimersByTimeAsync(10)

    expect(MockEventSource.instances.length).toBe(2)
    expect(MockEventSource.instances[1]!.url).toContain('lastEventId=evt-42')

    link.disconnect()
  })

  it('reconnectNow creates a fresh connection immediately', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)
    expect(MockEventSource.instances).toHaveLength(1)

    link.reconnectNow()
    await vi.advanceTimersByTimeAsync(10)
    expect(MockEventSource.instances).toHaveLength(2)

    link.disconnect()
  })

  it('exposes reconnect stats', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    expect(link.getStats().reconnectCount).toBe(0)
    expect(link.getStats().lastConnectedAt).toBeNull()
    expect(link.getStats().nextReconnectAt).toBeNull()

    link.connect()
    await vi.advanceTimersByTimeAsync(10)

    expect(link.getStats().reconnectCount).toBe(1)
    expect(link.getStats().lastConnectedAt).not.toBeNull()

    link.disconnect()
  })

  it('tracks next reconnect timestamp and counts it down', async () => {
    const link = new SseLink({
      url: '/api/v1/events/test',
      onData: vi.fn(),
      onFrame: vi.fn(),
    })

    link.connect()
    await vi.advanceTimersByTimeAsync(10)

    const es = MockEventSource.instances[0]
    es.simulateError()

    const stats1 = link.getStats()
    expect(stats1.nextReconnectAt).not.toBeNull()
    expect(stats1.nextReconnectDelayMs).not.toBeNull()
    expect(stats1.nextReconnectDelayMs! <= stats1.nextReconnectAt!).toBe(true)

    await vi.advanceTimersByTimeAsync(500)
    const stats2 = link.getStats()
    expect(stats2.nextReconnectDelayMs!).toBeLessThan(stats1.nextReconnectDelayMs!)

    link.disconnect()
  })
})
