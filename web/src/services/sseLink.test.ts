import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { SseLink } from './sseLink'

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

  simulateEvent(type: string, data: string) {
    this.listeners[type]?.({ data } as MessageEvent)
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

    await vi.advanceTimersByTimeAsync(1000)
    expect(MockEventSource.instances).toHaveLength(2)

    link.disconnect()
  })
})
