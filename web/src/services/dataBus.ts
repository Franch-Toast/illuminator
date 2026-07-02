/**
 * DataBus — 前端 SSE 数据总线
 *
 * 替代 LiveDataSource 的 Link Chain 架构，使用 SSE 单连接接收所有 Feature 数据。
 * 同时实现 DataSource 接口，让现有页面 hooks 无缝迁移。
 */

import { SseLink, SseFrame } from './sseLink'
import type { DataBatch, DataCallback as DsCallback, DataSource, ConnectionStatus } from './dataSource'

export type DataCallback = (data: unknown) => void

interface FeatureSubscription {
  callbacks: Set<DataCallback>
  ringBuffer: DataBatch[]
  maxBufferSize: number
}

interface SubscribeResponse {
  subscription_id: string
  url: string
}

interface PendingFrame {
  frames: Map<number, string>
  total: number
  feature: string
  createdAt: number
}

const FRAME_TIMEOUT_MS = 10000
const DEFAULT_RING_BUFFER_SIZE = 300

export class DataBus implements DataSource {
  private sseLink: SseLink | null = null
  private subscriptionId: string | null = null
  private subs = new Map<string, FeatureSubscription>()
  private pendingFrames = new Map<number, PendingFrame>()
  private frameCleanupTimer: ReturnType<typeof setInterval> | null = null
  private baseUrl: string
  private statusListeners = new Set<(s: ConnectionStatus) => void>()
  private status: ConnectionStatus = 'disconnected'
  private windowSeconds = 60

  constructor(baseUrl = '') {
    this.baseUrl = baseUrl
  }

  /**
   * DataSource interface: subscribe to feature data
   */
  subscribe(feature: string, cb: DsCallback): () => void {
    let sub = this.subs.get(feature)
    if (!sub) {
      sub = {
        callbacks: new Set(),
        ringBuffer: [],
        maxBufferSize: DEFAULT_RING_BUFFER_SIZE,
      }
      this.subs.set(feature, sub)
      this.syncSubscription()
    }
    sub.callbacks.add(cb as DataCallback)

    return () => {
      sub!.callbacks.delete(cb as DataCallback)
      if (sub!.callbacks.size === 0) {
        this.subs.delete(feature)
        this.syncSubscription()
      }
    }
  }

  getLatest(feature: string): DataBatch | null {
    const buf = this.subs.get(feature)?.ringBuffer
    return buf && buf.length > 0 ? buf[buf.length - 1] : null
  }

  getAvailableFeatures(): string[] {
    return Array.from(this.subs.keys())
  }

  destroy(): void {
    this.disconnect()
    this.subs.clear()
    this.statusListeners.clear()
  }

  getStatus(): ConnectionStatus {
    return this.status
  }

  onConnectionChange(listener: (s: ConnectionStatus) => void): () => void {
    this.statusListeners.add(listener)
    return () => { this.statusListeners.delete(listener) }
  }

  /**
   * RFC 5.1: 获取最近 N 条数据（前端导出用）
   */
  getRecent(feature: string, count: number): DataBatch[] {
    const buf = this.subs.get(feature)?.ringBuffer ?? []
    return buf.slice(-count)
  }

  /**
   * RFC 5.1: 设置时间窗口大小
   */
  setWindowSize(seconds: number): void {
    this.windowSeconds = seconds
    const newSize = Math.ceil(seconds * 2)
    for (const sub of this.subs.values()) {
      sub.maxBufferSize = newSize
      if (sub.ringBuffer.length > newSize) {
        sub.ringBuffer = sub.ringBuffer.slice(-newSize)
      }
    }
  }

  getWindowSize(): number {
    return this.windowSeconds
  }

  /**
   * 获取 Feature 的 ringBuffer 数据（用于前端保存）
   */
  getBuffer(feature: string): DataBatch[] {
    return this.subs.get(feature)?.ringBuffer ?? []
  }

  async connect(): Promise<void> {
    this.setStatus('connecting')

    const features = Array.from(this.subs.keys())

    const resp = await fetch(`${this.baseUrl}/api/v1/events/subscribe`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ features }),
    })

    if (!resp.ok) {
      this.setStatus('disconnected')
      throw new Error(`Subscribe failed: ${resp.status}`)
    }
    const data: SubscribeResponse = await resp.json()
    this.subscriptionId = data.subscription_id

    this.sseLink = new SseLink({
      url: `${this.baseUrl}${data.url}`,
      onData: (feature, payload) => this.handleData(feature, payload),
      onFrame: (frame) => this.handleFrame(frame),
      onOpen: () => this.setStatus('connected'),
      onError: () => this.setStatus('disconnected'),
    })

    this.sseLink.connect()
    this.startFrameCleanup()

    // Sync any features that were added during the await (race condition fix)
    const currentFeatures = Array.from(this.subs.keys())
    const newFeatures = currentFeatures.filter(f => !features.includes(f))
    if (newFeatures.length > 0) {
      this.syncSubscription()
    }
  }

  disconnect(): void {
    this.stopFrameCleanup()
    this.sseLink?.disconnect()
    this.sseLink = null
    this.subscriptionId = null
    this.pendingFrames.clear()
    this.setStatus('disconnected')
  }

  get connected(): boolean {
    return this.sseLink?.connected ?? false
  }

  private setStatus(s: ConnectionStatus): void {
    if (this.status === s) return
    this.status = s
    for (const cb of this.statusListeners) cb(s)
  }

  private async syncSubscription(): Promise<void> {
    if (!this.subscriptionId) return

    const currentFeatures = Array.from(this.subs.keys())

    try {
      await fetch(`${this.baseUrl}/api/v1/events/${this.subscriptionId}/update`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          add: currentFeatures,
          remove: [],
        }),
      })
    } catch {
      // Silently ignore sync failures
    }
  }

  private handleData(feature: string, payload: unknown): void {
    const sub = this.subs.get(feature)
    if (!sub) return

    const batch: DataBatch = {
      feature,
      timestamp: (payload as Record<string, unknown>)?.timestamp as number ?? Date.now(),
      modelType: (payload as Record<string, unknown>)?.modelType as DataBatch['modelType'],
      data: payload,
    }

    sub.ringBuffer.push(batch)
    if (sub.ringBuffer.length > sub.maxBufferSize) {
      sub.ringBuffer.shift()
    }

    for (const cb of sub.callbacks) {
      try { (cb as DsCallback)(batch) } catch { /* subscriber error */ }
    }
  }

  private handleFrame(frame: SseFrame): void {
    let pending = this.pendingFrames.get(frame.seq)
    if (!pending) {
      pending = {
        frames: new Map(),
        total: frame.frame_total,
        feature: frame.feature,
        createdAt: Date.now(),
      }
      this.pendingFrames.set(frame.seq, pending)
    }

    pending.frames.set(frame.frame_idx, frame.payload)

    if (pending.frames.size === pending.total) {
      const parts: string[] = []
      for (let i = 0; i < pending.total; i++) {
        parts.push(pending.frames.get(i) || '')
      }
      const fullJson = parts.join('')
      this.pendingFrames.delete(frame.seq)

      try {
        const data = JSON.parse(fullJson)
        this.handleData(pending.feature, data)
      } catch {
        // Discard malformed reassembled data
      }
    }
  }

  private startFrameCleanup(): void {
    this.frameCleanupTimer = setInterval(() => {
      const now = Date.now()
      for (const [seq, pending] of this.pendingFrames) {
        if (now - pending.createdAt > FRAME_TIMEOUT_MS) {
          this.pendingFrames.delete(seq)
        }
      }
    }, 5000)
  }

  private stopFrameCleanup(): void {
    if (this.frameCleanupTimer) {
      clearInterval(this.frameCleanupTimer)
      this.frameCleanupTimer = null
    }
  }
}

export const dataBus = new DataBus()
