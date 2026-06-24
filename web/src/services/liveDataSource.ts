import type { DataBatch, DataCallback, DataSource, ConnectionStatus, DataSourceEvents } from './dataSource'
import { WsLink } from './links/WsLink'
import { HttpLink } from './links/HttpLink'

type FeatureSubscription = {
  callbacks: Set<DataCallback>
  latest: DataBatch | null
}

/**
 * LiveDataSource — Link Chain architecture.
 *
 * Uses WsLink as the primary data channel (real-time push).
 * Falls back to HttpLink (polling) when WS is disconnected.
 * Visibility-aware: pauses polling when page is hidden.
 */
export class LiveDataSource implements DataSource {
  private subs = new Map<string, FeatureSubscription>()
  private wsLink: WsLink
  private httpLink: HttpLink
  private status: ConnectionStatus = 'disconnected'
  private events: DataSourceEvents
  private visible = true
  private visibilityHandler: (() => void) | null = null

  constructor(events: DataSourceEvents = {}, pollIntervalMs = 1000) {
    this.events = events

    const handleData = (batch: DataBatch) => {
      const sub = this.subs.get(batch.feature)
      if (sub) {
        sub.latest = batch
        for (const cb of sub.callbacks) cb(batch)
      }
    }

    const handleStatus = (s: ConnectionStatus) => {
      if (this.status === s) return
      this.status = s
      this.events.onConnectionChange?.(s)

      if (s === 'connected') {
        this.httpLink.disconnect()
      } else if (s === 'disconnected' && this.visible) {
        this.activateHttpFallback()
      }
    }

    this.wsLink = new WsLink(this.getWsUrl(), handleData, handleStatus)
    this.httpLink = new HttpLink(handleData, pollIntervalMs)

    this.setupVisibility()
    this.wsLink.connect()
  }

  subscribe(feature: string, cb: DataCallback): () => void {
    let sub = this.subs.get(feature)
    if (!sub) {
      sub = { callbacks: new Set(), latest: null }
      this.subs.set(feature, sub)
    }
    sub.callbacks.add(cb)

    if (this.wsLink.isConnected()) {
      this.wsLink.subscribe(feature)
    } else {
      this.httpLink.connect()
      this.httpLink.subscribe(feature)
    }

    return () => {
      sub!.callbacks.delete(cb)
      if (sub!.callbacks.size === 0) {
        this.wsLink.unsubscribe(feature)
        this.httpLink.unsubscribe(feature)
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

  getStatus(): ConnectionStatus {
    return this.status
  }

  destroy(): void {
    if (this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler)
    }
    this.wsLink.disconnect()
    this.httpLink.disconnect()
    this.subs.clear()
  }

  private getWsUrl(): string {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const host = window.location.host
    return `${protocol}//${host}/ws/features`
  }

  private setupVisibility() {
    this.visibilityHandler = () => {
      const wasVisible = this.visible
      this.visible = document.visibilityState === 'visible'

      if (!wasVisible && this.visible) {
        if (!this.wsLink.isConnected()) {
          this.wsLink.connect()
          this.activateHttpFallback()
        }
      } else if (wasVisible && !this.visible) {
        this.httpLink.disconnect()
      }
    }
    document.addEventListener('visibilitychange', this.visibilityHandler)
  }

  private activateHttpFallback() {
    this.httpLink.connect()
    for (const feature of this.subs.keys()) {
      this.httpLink.subscribe(feature)
    }
  }
}
