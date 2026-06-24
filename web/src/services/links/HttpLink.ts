import type { DataBatch } from '../dataSource'
import type { DataLink, DataHandler } from './types'
import { api } from '../apiClient'

export class HttpLink implements DataLink {
  readonly name = 'http'
  private timers = new Map<string, ReturnType<typeof setInterval>>()
  private onData: DataHandler
  private intervalMs: number
  private active = false

  constructor(onData: DataHandler, intervalMs = 1000) {
    this.onData = onData
    this.intervalMs = intervalMs
  }

  connect(): void {
    this.active = true
  }

  disconnect(): void {
    this.active = false
    for (const [, timer] of this.timers) clearInterval(timer)
    this.timers.clear()
  }

  subscribe(feature: string): void {
    if (this.timers.has(feature)) return
    const poll = async () => {
      if (!this.active) return
      try {
        const resp = await api.featureCollect(feature)
        if (resp && typeof resp === 'object') {
          const batch: DataBatch = { feature, timestamp: Date.now(), modelType: 'generic', data: resp }
          this.onData(batch)
        }
      } catch { /* retry on next interval */ }
    }
    poll()
    this.timers.set(feature, setInterval(poll, this.intervalMs))
  }

  unsubscribe(feature: string): void {
    const timer = this.timers.get(feature)
    if (timer) {
      clearInterval(timer)
      this.timers.delete(feature)
    }
  }

  isConnected(): boolean {
    return this.active
  }
}
