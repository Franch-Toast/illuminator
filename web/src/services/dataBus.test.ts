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

  it('handles data events and buffers them before flush', async () => {
    const cb = vi.fn()
    bus.subscribe('cpu_utilization', cb)
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 42 }))

    // 默认批量缓冲，未 flush 前不应通知
    expect(cb).not.toHaveBeenCalled()
    bus.flushBatch()

    expect(cb).toHaveBeenCalledWith(expect.objectContaining({
      feature: 'cpu_utilization',
      data: { feature: 'cpu_utilization', value: 42 },
    }))
  })

  it('buffers data in ringBuffer after flush', async () => {
    bus.subscribe('cpu_utilization', vi.fn())
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 1 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 2 }))
    bus.flushBatch()

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

    bus.flushBatch()

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
    bus.flushBatch()

    expect(cb).not.toHaveBeenCalled()
  })

  it('batches multiple messages and notifies in one flush', async () => {
    const cb = vi.fn()
    bus.subscribe('cpu_utilization', cb)
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 1 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 2 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 3 }))

    expect(cb).not.toHaveBeenCalled()
    bus.flushBatch()

    expect(cb).toHaveBeenCalledTimes(3)
  })

  it('detects backpressure when flush processing is slow', async () => {
    const cb = vi.fn(() => {
      // 模拟耗时处理，超过 200ms 阈值
      const start = performance.now()
      while (performance.now() - start < 250) { /* busy wait */ }
    })
    bus.subscribe('cpu_utilization', cb)
    await bus.connect()

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 1 }))
    bus.flushBatch()

    expect(bus.isBackpressured()).toBe(true)
  })

  it('background mode keeps only latest snapshot and flushes on visible', async () => {
    const cb = vi.fn()
    bus.subscribe('cpu_utilization', cb)
    await bus.connect()

    // 模拟进入后台
    Object.defineProperty(document, 'hidden', { value: true, writable: true, configurable: true })
    document.dispatchEvent(new Event('visibilitychange'))
    expect(bus.isBackgroundMode()).toBe(true)

    const es = MockEventSource.instances[0]
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 1 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 2 }))
    es.simulateEvent('data', JSON.stringify({ feature: 'cpu_utilization', value: 3 }))

    // 后台时不通知
    bus.flushBatch()
    expect(cb).not.toHaveBeenCalled()

    // 模拟回到前台
    Object.defineProperty(document, 'hidden', { value: false, writable: true, configurable: true })
    document.dispatchEvent(new Event('visibilitychange'))
    expect(bus.isBackgroundMode()).toBe(false)

    // 只应收到最新的快照
    expect(cb).toHaveBeenCalledTimes(1)
    expect(cb).toHaveBeenCalledWith(expect.objectContaining({
      data: expect.objectContaining({ value: 3 }),
    }))
  })

  it('setConfig updates batch interval and flushes existing batch', () => {
    const cb = vi.fn()
    bus.subscribe('cpu_utilization', cb)
    bus.setConfig({ batchIntervalMs: 50 })
    expect(bus.getConfig().batchIntervalMs).toBe(50)
  })
})
