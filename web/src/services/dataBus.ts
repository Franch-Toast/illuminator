/**
 * DataBus — 前端 SSE 数据总线
 *
 * 替代 LiveDataSource 的 Link Chain 架构，使用 SSE 单连接接收所有 Feature 数据。
 * 同时实现 DataSource 接口，让现有页面 hooks 无缝迁移。
 *
 * 优化点：
 * - 批量缓冲：100ms 内的消息合并为一个 rAF 帧内通知，降低 React re-render 次数。
 * - 逆向背压检测：上一批处理耗时 >200ms 时设置 backpressured 标志。
 * - Page Visibility 感知：后台只保留最新快照，切回前台立即 flush。
 */

import { SseLink, SseFrame, ConnectionState } from './sseLink'
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

export interface DataBusConfig {
  batchIntervalMs: number
  backpressureThresholdMs: number
}

const FRAME_TIMEOUT_MS = 10000
const DEFAULT_RING_BUFFER_SIZE = 300
const DEFAULT_BATCH_INTERVAL_MS = 100
const DEFAULT_BACKPRESSURE_THRESHOLD_MS = 200

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

  // 批量通知状态
  private pendingBatches = new Map<string, DataBatch[]>()
  private batchTimer: ReturnType<typeof setTimeout> | null = null
  private rafId: number | null = null
  private lastBatchProcessTime = 0
  private backpressured = false
  private backgroundMode = false
  private config: DataBusConfig

  constructor(baseUrl = '', config: Partial<DataBusConfig> = {}) {
    this.baseUrl = baseUrl
    this.config = {
      batchIntervalMs: config.batchIntervalMs ?? DEFAULT_BATCH_INTERVAL_MS,
      backpressureThresholdMs: config.backpressureThresholdMs ?? DEFAULT_BACKPRESSURE_THRESHOLD_MS,
    }
    this.bindVisibilityListener()
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
        this.pendingBatches.delete(feature)
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
    this.pendingBatches.clear()
    this.statusListeners.clear()
    this.removeVisibilityListener()
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

  /**
   * 批量通知配置
   */
  setConfig(config: Partial<DataBusConfig>): void {
    this.config = { ...this.config, ...config }
    // 配置变更时立即刷新当前批次，避免旧间隔造成不可预期的延迟
    this.flushBatch()
  }

  getConfig(): DataBusConfig {
    return { ...this.config }
  }

  /**
   * 当前是否处于背压状态
   */
  isBackpressured(): boolean {
    return this.backpressured
  }

  /**
   * 当前是否处于后台模式
   */
  isBackgroundMode(): boolean {
    return this.backgroundMode
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
      onError: () => this.setStatus('error'),
      onStateChange: (state) => this.setStatus(mapConnectionState(state)),
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
    this.cancelBatchFlush()
    this.sseLink?.disconnect()
    this.sseLink = null
    this.subscriptionId = null
    this.pendingFrames.clear()
    this.pendingBatches.clear()
    this.backpressured = false
    this.setStatus('disconnected')
  }

  get connected(): boolean {
    return this.sseLink?.connected ?? false
  }

  reconnectNow(): void {
    this.sseLink?.reconnectNow()
  }

  getConnectionStats(): { reconnectCount: number; lastConnectedAt: number | null; nextReconnectDelayMs: number | null; nextReconnectAt: number | null } {
    const stats = this.sseLink?.getStats()
    return {
      reconnectCount: stats?.reconnectCount ?? 0,
      lastConnectedAt: stats?.lastConnectedAt ?? null,
      nextReconnectDelayMs: stats?.nextReconnectDelayMs ?? null,
      nextReconnectAt: stats?.nextReconnectAt ?? null,
    }
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

    if (this.backgroundMode) {
      // 后台模式：ringBuffer 保持最新，pending 只保留最新快照
      this.storeInRingBuffer(sub, batch)
      this.pendingBatches.set(feature, [batch])
      return
    }

    let queue = this.pendingBatches.get(feature)
    if (!queue) {
      queue = []
      this.pendingBatches.set(feature, queue)
    }
    queue.push(batch)

    this.scheduleBatchFlush()
  }

  private storeInRingBuffer(sub: FeatureSubscription, batch: DataBatch): void {
    sub.ringBuffer.push(batch)
    if (sub.ringBuffer.length > sub.maxBufferSize) {
      sub.ringBuffer.shift()
    }
  }

  private scheduleBatchFlush(): void {
    if (this.batchTimer) return
    this.batchTimer = setTimeout(() => {
      this.batchTimer = null
      this.scheduleRAF()
    }, this.config.batchIntervalMs)
  }

  private scheduleRAF(): void {
    if (this.rafId !== null) return
    this.rafId = requestAnimationFrame(() => {
      this.rafId = null
      this.flushBatch()
    })
  }

  private cancelBatchFlush(): void {
    if (this.batchTimer) {
      clearTimeout(this.batchTimer)
      this.batchTimer = null
    }
    if (this.rafId !== null) {
      cancelAnimationFrame(this.rafId)
      this.rafId = null
    }
  }

  /**
   * 立即刷新批量缓冲。用于切回前台时立即推送最新数据。
   */
  flushBatch(): void {
    if (this.pendingBatches.size === 0) return
    // 后台模式下保留 pendingBatches 中的最新快照，待切回前台后再 flush 通知。
    if (this.backgroundMode) return

    const start = performance.now()
    const batches = this.pendingBatches
    this.pendingBatches = new Map()

    for (const [feature, queue] of batches) {
      const sub = this.subs.get(feature)
      if (!sub) continue

      for (const batch of queue) {
        this.storeInRingBuffer(sub, batch)
      }

      // 后台模式下只保留最新快照，切回前台前不通知订阅者
      if (this.backgroundMode) continue

      for (const batch of queue) {
        for (const cb of sub.callbacks) {
          try { (cb as DsCallback)(batch) } catch { /* subscriber error */ }
        }
      }
    }

    this.lastBatchProcessTime = performance.now() - start
    this.backpressured = this.lastBatchProcessTime > this.config.backpressureThresholdMs
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

  private visibilityHandler = (): void => {
    if (document.hidden) {
      this.backgroundMode = true
      this.cancelBatchFlush()
    } else {
      this.backgroundMode = false
      this.flushBatch()
    }
  }

  private bindVisibilityListener(): void {
    if (typeof document === 'undefined') return
    document.addEventListener('visibilitychange', this.visibilityHandler)
  }

  private removeVisibilityListener(): void {
    if (typeof document === 'undefined') return
    document.removeEventListener('visibilitychange', this.visibilityHandler)
  }
}

export const dataBus = new DataBus()

function mapConnectionState(state: ConnectionState): ConnectionStatus {
  switch (state) {
    case ConnectionState.Connected:
      return 'connected'
    case ConnectionState.Reconnecting:
      return 'reconnecting'
    case ConnectionState.Stale:
      return 'stale'
    case ConnectionState.Error:
      return 'error'
    case ConnectionState.Disconnected:
    default:
      return 'disconnected'
  }
}
