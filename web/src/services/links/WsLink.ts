import type { DataBatch, DataModelType } from '../dataSource'
import type { DataLink, DataHandler, StatusHandler } from './types'

function inferModelType(msg: Record<string, unknown>): DataModelType {
  if (msg.model_type && typeof msg.model_type === 'string') return msg.model_type as DataModelType
  if (msg.stack_samples || msg.type === 'notify') return 'profile'
  if (msg.spans) return 'trace'
  if (msg.log_lines) return 'log'
  if (msg.records || msg.samples) return 'time_series'
  return 'generic'
}

export class WsLink implements DataLink {
  readonly name = 'ws'
  private ws: WebSocket | null = null
  private retries = 0
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private features = new Set<string>()
  private onData: DataHandler
  private onStatus: StatusHandler
  private url: string

  constructor(url: string, onData: DataHandler, onStatus: StatusHandler) {
    this.url = url
    this.onData = onData
    this.onStatus = onStatus
  }

  connect(): void {
    if (this.ws && this.ws.readyState <= WebSocket.OPEN) return

    try {
      this.onStatus('connecting')
      this.ws = new WebSocket(this.url)

      this.ws.onopen = () => {
        this.retries = 0
        this.onStatus('connected')
        for (const feature of this.features) {
          this.sendSubscribe(feature)
        }
      }

      this.ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data)
          const feature = msg.pipeline || msg.feature
          if (!feature) return
          const batch: DataBatch = { feature, timestamp: msg.ts || Date.now(), modelType: inferModelType(msg), data: msg }
          this.onData(batch)
        } catch { /* ignore parse errors */ }
      }

      this.ws.onclose = () => {
        this.ws = null
        this.onStatus('disconnected')
        this.scheduleReconnect()
      }

      this.ws.onerror = () => { this.ws?.close() }
    } catch {
      this.onStatus('disconnected')
      this.scheduleReconnect()
    }
  }

  disconnect(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    if (this.ws) {
      this.ws.onclose = null
      this.ws.close()
      this.ws = null
    }
  }

  subscribe(feature: string): void {
    this.features.add(feature)
    this.sendSubscribe(feature)
  }

  unsubscribe(feature: string): void {
    this.features.delete(feature)
  }

  isConnected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN
  }

  private sendSubscribe(feature: string) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(`subscribe:${feature}`)
    }
  }

  private scheduleReconnect() {
    if (this.reconnectTimer) return
    const delay = Math.min(1000 * Math.pow(2, this.retries), 30000)
    this.retries++
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.connect()
    }, delay)
  }
}
