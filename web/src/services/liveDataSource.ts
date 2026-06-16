import type { DataBatch, DataCallback, DataSource, ConnectionStatus, DataSourceEvents } from './dataSource'
import { api } from './apiClient'

type FeatureSubscription = {
  callbacks: Set<DataCallback>
  latest: DataBatch | null
  pollTimer: ReturnType<typeof setInterval> | null
}

export class LiveDataSource implements DataSource {
  private subs = new Map<string, FeatureSubscription>()
  private ws: WebSocket | null = null
  private wsRetries = 0
  private wsReconnectTimer: ReturnType<typeof setTimeout> | null = null
  private status: ConnectionStatus = 'disconnected'
  private events: DataSourceEvents
  private visible = true
  private visibilityHandler: (() => void) | null = null
  private pollIntervalMs: number

  constructor(events: DataSourceEvents = {}, pollIntervalMs = 1000) {
    this.events = events
    this.pollIntervalMs = pollIntervalMs
    this.setupVisibility()
    this.connectWs()
  }

  subscribe(feature: string, cb: DataCallback): () => void {
    let sub = this.subs.get(feature)
    if (!sub) {
      sub = { callbacks: new Set(), latest: null, pollTimer: null }
      this.subs.set(feature, sub)
    }
    sub.callbacks.add(cb)

    if (this.status !== 'connected') {
      this.startPolling(feature, sub)
    } else {
      this.sendWsSubscribe(feature)
    }

    return () => {
      sub!.callbacks.delete(cb)
      if (sub!.callbacks.size === 0) {
        this.stopPolling(sub!)
        this.subs.delete(feature)
      }
    }
  }

  getLatest(feature: string): DataBatch | null {
    return this.subs.get(feature)?.latest ?? null
  }

  getAvailableFeatures(): string[] {
    return Array.from(this.subs.keys())
  }

  destroy(): void {
    if (this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler)
    }
    for (const [, sub] of this.subs) {
      this.stopPolling(sub)
    }
    this.subs.clear()
    this.disconnectWs()
  }

  getStatus(): ConnectionStatus {
    return this.status
  }

  private setupVisibility() {
    this.visibilityHandler = () => {
      const wasVisible = this.visible
      this.visible = document.visibilityState === 'visible'

      if (!wasVisible && this.visible) {
        this.onVisible()
      } else if (wasVisible && !this.visible) {
        this.onHidden()
      }
    }
    document.addEventListener('visibilitychange', this.visibilityHandler)
  }

  private onVisible() {
    if (this.status !== 'connected') {
      this.connectWs()
    }
    for (const [feature, sub] of this.subs) {
      if (this.status !== 'connected') {
        this.startPolling(feature, sub)
      }
    }
  }

  private onHidden() {
    for (const [, sub] of this.subs) {
      this.stopPolling(sub)
    }
  }

  private getWsUrl(): string {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const hostname = window.location.hostname
    const httpPort = parseInt(window.location.port || (protocol === 'wss:' ? '443' : '80'), 10)
    const wsPort = httpPort + 1
    return `${protocol}//${hostname}:${wsPort}/ws/features`
  }

  private connectWs() {
    if (this.ws && this.ws.readyState <= WebSocket.OPEN) return

    const url = this.getWsUrl()

    try {
      this.setStatus('connecting')
      this.ws = new WebSocket(url)

      this.ws.onopen = () => {
        this.wsRetries = 0
        this.setStatus('connected')
        for (const [, sub] of this.subs) {
          this.stopPolling(sub)
        }
        for (const feature of this.subs.keys()) {
          this.sendWsSubscribe(feature)
        }
      }

      this.ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data)
          const feature = msg.pipeline || msg.feature
          if (!feature) return

          const batch: DataBatch = {
            feature,
            timestamp: msg.ts || Date.now(),
            data: msg,
          }

          const sub = this.subs.get(feature)
          if (sub) {
            sub.latest = batch
            for (const cb of sub.callbacks) cb(batch)
          }
        } catch { /* ignore parse errors */ }
      }

      this.ws.onclose = () => {
        this.ws = null
        this.setStatus('disconnected')
        this.fallbackToPolling()
        this.scheduleReconnect()
      }

      this.ws.onerror = () => {
        this.ws?.close()
      }
    } catch {
      this.setStatus('disconnected')
      this.fallbackToPolling()
      this.scheduleReconnect()
    }
  }

  private disconnectWs() {
    if (this.wsReconnectTimer) {
      clearTimeout(this.wsReconnectTimer)
      this.wsReconnectTimer = null
    }
    if (this.ws) {
      this.ws.onclose = null
      this.ws.close()
      this.ws = null
    }
  }

  private scheduleReconnect() {
    if (this.wsReconnectTimer) return
    const delay = Math.min(1000 * Math.pow(2, this.wsRetries), 30000)
    this.wsRetries++
    this.wsReconnectTimer = setTimeout(() => {
      this.wsReconnectTimer = null
      if (this.visible) this.connectWs()
    }, delay)
  }

  private sendWsSubscribe(feature: string) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(`subscribe:${feature}`)
    }
  }

  private fallbackToPolling() {
    if (!this.visible) return
    for (const [feature, sub] of this.subs) {
      if (!sub.pollTimer) {
        this.startPolling(feature, sub)
      }
    }
  }

  private startPolling(feature: string, sub: FeatureSubscription) {
    if (sub.pollTimer) return
    const interval = this.visible ? this.pollIntervalMs : this.pollIntervalMs * 5

    const poll = async () => {
      try {
        const resp = await api.featureCollect(feature)
        if (resp && typeof resp === 'object') {
          const batch: DataBatch = {
            feature,
            timestamp: Date.now(),
            data: resp,
          }
          sub.latest = batch
          for (const cb of sub.callbacks) cb(batch)
        }
      } catch { /* retry on next interval */ }
    }

    poll()
    sub.pollTimer = setInterval(poll, interval)
  }

  private stopPolling(sub: FeatureSubscription) {
    if (sub.pollTimer) {
      clearInterval(sub.pollTimer)
      sub.pollTimer = null
    }
  }

  private setStatus(s: ConnectionStatus) {
    if (this.status === s) return
    this.status = s
    this.events.onConnectionChange?.(s)
  }
}
