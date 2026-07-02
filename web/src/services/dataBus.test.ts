import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { DataBus } from './dataBus'

class MockEventSource {
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
      this.readyState = 1
      this.onopen?.()
    }, 5)
  }

  addEventListener(event: string, handler: (e: MessageEvent) => void) {
    this.listeners[event] = handler
  }

  close = vi.fn()

  simulateEvent(type: string, data: string) {
    this.listeners[type]?.({ data } as MessageEvent)
  }
}

globalThis.EventSource = MockEventSource as unknown as typeof EventSource

globalThis.fetch = vi.fn().mockImplementation((url: string) => {
  if (url.includes('/subscribe')) {
    return Promise.resolve({
      ok: true,
      json: () => Promise.resolve({ subscription_id: 'sub_test', url: '/api/v1/events/sub_test' }),
    })
  }
  if (url.includes('/update')) {
    return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) })
  }
  return Promise.resolve({ ok: false })
}) as typeof fetch

describe('DataBus', () => {
  let bus: DataBus

  beforeEach(() => {
    MockEventSource.instances = []
    bus = new DataBus('')
  })

  afterEach(() => {
    bus.disconnect()
  })

  it('subscribe returns unsubscribe function', () => {
    const cb = vi.fn()
    const unsub = bus.subscribe('cpu_utilization', cb)
    expect(typeof unsub).toBe('function')
    unsub()
  })

  it('getBuffer returns empty array initially', () => {
    bus.subscribe('cpu_utilization', vi.fn())
    expect(bus.getBuffer('cpu_utilization')).toEqual([])
  })

  it('connect creates SSE connection', async () => {
    bus.subscribe('cpu_utilization', vi.fn())
    await bus.connect()
    expect(MockEventSource.instances.length).toBe(1)
    expect(MockEventSource.instances[0].url).toContain('sub_test')
  })

  it('disconnect closes connection', async () => {
    bus.subscribe('cpu_utilization', vi.fn())
    await bus.connect()
    bus.disconnect()
    expect(bus.connected).toBe(false)
  })

  it('handles data events and notifies subscribers', async () => {
    const cb = vi.fn()
    bus.subscribe('cpu_utilization', cb)
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 42 }))

    expect(cb).toHaveBeenCalledWith(expect.objectContaining({
      feature: 'cpu_utilization',
      data: { feature: 'cpu_utilization', value: 42 },
    }))
  })

  it('buffers data in ringBuffer', async () => {
    bus.subscribe('cpu_utilization', vi.fn())
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 1 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 2 }))

    const buffer = bus.getBuffer('cpu_utilization')
    expect(buffer).toHaveLength(2)
  })

  it('handles frame reassembly', async () => {
    const cb = vi.fn()
    bus.subscribe('cpu_profiler', cb)
    await bus.connect()

    const es = MockEventSource.instances[0]

    es.simulateEvent('frame', JSON.stringify({
      feature: 'cpu_profiler', seq: 1, frame_idx: 0, frame_total: 2,
      payload: '{"dat',
    }))
    es.simulateEvent('frame', JSON.stringify({
      feature: 'cpu_profiler', seq: 1, frame_idx: 1, frame_total: 2,
      payload: 'a":42}',
    }))

    expect(cb).toHaveBeenCalledWith(expect.objectContaining({
      feature: 'cpu_profiler',
      data: { data: 42 },
    }))
  })

  it('does not notify unsubscribed callbacks', async () => {
    const cb = vi.fn()
    const unsub = bus.subscribe('cpu_utilization', cb)
    await bus.connect()
    unsub()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 99 }))

    expect(cb).not.toHaveBeenCalled()
  })
})
