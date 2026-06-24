import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { WsLink } from './WsLink'

class MockWebSocket {
  static CONNECTING = 0
  static OPEN = 1
  static CLOSING = 2
  static CLOSED = 3

  readyState = MockWebSocket.CONNECTING
  onopen: (() => void) | null = null
  onmessage: ((e: { data: string }) => void) | null = null
  onclose: (() => void) | null = null
  onerror: (() => void) | null = null
  sentMessages: string[] = []

  send(msg: string) { this.sentMessages.push(msg) }
  close() { this.readyState = MockWebSocket.CLOSED; this.onclose?.() }

  simulateOpen() {
    this.readyState = MockWebSocket.OPEN
    this.onopen?.()
  }

  simulateMessage(data: string) {
    this.onmessage?.({ data })
  }
}

let mockWsInstance: MockWebSocket | null = null

describe('WsLink', () => {
  beforeEach(() => {
    mockWsInstance = null
    vi.stubGlobal('WebSocket', class {
      constructor() {
        mockWsInstance = new MockWebSocket()
        return mockWsInstance as unknown as WebSocket
      }
      static OPEN = 1
    })
  })

  afterEach(() => { vi.restoreAllMocks() })

  it('connects and reports connected status', () => {
    const onStatus = vi.fn()
    const link = new WsLink('ws://test/ws', vi.fn(), onStatus)
    link.connect()
    expect(onStatus).toHaveBeenCalledWith('connecting')
    mockWsInstance!.simulateOpen()
    expect(onStatus).toHaveBeenCalledWith('connected')
  })

  it('sends subscribe messages for registered features on open', () => {
    const link = new WsLink('ws://test/ws', vi.fn(), vi.fn())
    link.subscribe('cpu_utilization')
    link.connect()
    mockWsInstance!.simulateOpen()
    expect(mockWsInstance!.sentMessages).toContain('subscribe:cpu_utilization')
  })

  it('dispatches parsed messages to onData', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'cpu_utilization', ts: 1000, value: 42 }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({
      feature: 'cpu_utilization',
      timestamp: 1000,
    }))
  })

  it('infers modelType as time_series for records payload', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'cpu', ts: 100, records: [{ cpu: 50 }] }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ modelType: 'time_series' }))
  })

  it('infers modelType as profile for stack_samples payload', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'cpu_profile', ts: 100, stack_samples: [] }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ modelType: 'profile' }))
  })

  it('infers modelType as trace for spans payload', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'sched', ts: 100, spans: [] }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ modelType: 'trace' }))
  })

  it('infers modelType as log for log_lines payload', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'syslog', ts: 100, log_lines: ['hello'] }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ modelType: 'log' }))
  })

  it('respects explicit model_type field', () => {
    const onData = vi.fn()
    const link = new WsLink('ws://test/ws', onData, vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    mockWsInstance!.simulateMessage(JSON.stringify({ feature: 'custom', ts: 100, model_type: 'trace', data: {} }))
    expect(onData).toHaveBeenCalledWith(expect.objectContaining({ modelType: 'trace' }))
  })

  it('isConnected returns true when WS is open', () => {
    const link = new WsLink('ws://test/ws', vi.fn(), vi.fn())
    link.connect()
    expect(link.isConnected()).toBe(false)
    mockWsInstance!.simulateOpen()
    expect(link.isConnected()).toBe(true)
  })

  it('disconnect cleans up', () => {
    const link = new WsLink('ws://test/ws', vi.fn(), vi.fn())
    link.connect()
    mockWsInstance!.simulateOpen()
    link.disconnect()
    expect(link.isConnected()).toBe(false)
  })
})
