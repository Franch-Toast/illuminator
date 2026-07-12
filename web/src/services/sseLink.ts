/**
 * SSE Link — EventSource 封装
 *
 * 建立 SSE 连接并将事件分发给回调。支持自动重连（指数退避 + jitter）、
 * 心跳超时检测、连接状态事件、手动重连以及 Last-Event-ID 回补。
 */

export enum ConnectionState {
  Connected = 'connected',
  Reconnecting = 'reconnecting',
  Disconnected = 'disconnected',
  Error = 'error',
  Stale = 'stale',
}

export interface SseLinkOptions {
  url: string
  onData: (feature: string, data: unknown) => void
  onFrame: (frame: SseFrame) => void
  onError?: (error: Event) => void
  onOpen?: () => void
  onStateChange?: (state: ConnectionState) => void
}

export interface SseFrame {
  feature: string
  seq: number
  frame_idx: number
  frame_total: number
  payload: string
}

export interface SseLinkStats {
  state: ConnectionState
  reconnectCount: number
  lastConnectedAt: number | null
  lastEventId: string | null
  nextReconnectDelayMs: number | null
  nextReconnectAt: number | null
}

const DEFAULT_HEARTBEAT_TIMEOUT_MS = 30000
const RECONNECT_BACKOFF_DELAYS_MS = [1000, 2000, 4000, 8000, 16000, 30000]
const MAX_RECONNECT_DELAY_MS = 30000

/**
 * 对退避延迟施加随机抖动，防止大量客户端同时重连造成雷群效应。
 * Jitter 范围：±20%
 */
function applyJitter(delayMs: number): number {
  const jitter = delayMs * 0.2 * (Math.random() * 2 - 1)
  return Math.max(0, Math.round(delayMs + jitter))
}

export class SseLink {
  private es: EventSource | null = null
  private options: SseLinkOptions
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private heartbeatTimer: ReturnType<typeof setTimeout> | null = null
  private reconnectAttempt = 0
  private reconnectCount = 0
  private lastConnectedAt: number | null = null
  private lastEventId: string | null = null
  private nextReconnectAt: number | null = null
  private state = ConnectionState.Disconnected
  private heartbeatTimeoutMs: number

  constructor(options: SseLinkOptions, heartbeatTimeoutMs = DEFAULT_HEARTBEAT_TIMEOUT_MS) {
    this.options = options
    this.heartbeatTimeoutMs = heartbeatTimeoutMs
  }

  connect(): void {
    this.disconnect(false)

    let url = this.options.url
    // 手动重连时，若已有 Last-Event-ID，通过 query param 告知后端
    if (this.lastEventId) {
      const sep = url.includes('?') ? '&' : '?'
      url += `${sep}lastEventId=${encodeURIComponent(this.lastEventId)}`
    }

    this.es = new EventSource(url)
    this.setState(ConnectionState.Reconnecting)

    this.es.onopen = () => {
      this.reconnectAttempt = 0
      this.reconnectCount += 1
      this.lastConnectedAt = Date.now()
      this.setState(ConnectionState.Connected)
      this.resetHeartbeatTimer()
      this.options.onOpen?.()
    }

    this.es.onerror = (e) => {
      this.options.onError?.(e)
      this.setState(ConnectionState.Error)
      this.scheduleReconnect()
    }

    this.es.onmessage = (event: MessageEvent) => {
      this.resetHeartbeatTimer()
      if (event.lastEventId) {
        this.lastEventId = event.lastEventId
      }
    }

    this.es.addEventListener('data', (event: MessageEvent) => {
      this.resetHeartbeatTimer()
      if (event.lastEventId) {
        this.lastEventId = event.lastEventId
      }
      try {
        const parsed = JSON.parse(event.data)
        const feature = parsed.feature || ''
        this.options.onData(feature, parsed)
      } catch {
        // Ignore parse errors
      }
    })

    this.es.addEventListener('frame', (event: MessageEvent) => {
      this.resetHeartbeatTimer()
      if (event.lastEventId) {
        this.lastEventId = event.lastEventId
      }
      try {
        const frame: SseFrame = JSON.parse(event.data)
        this.options.onFrame(frame)
      } catch {
        // Ignore parse errors
      }
    })
  }

  disconnect(clearLastEventId = true): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.nextReconnectAt = null
    this.clearHeartbeatTimer()
    if (this.es) {
      this.es.close()
      this.es = null
    }
    if (clearLastEventId) {
      this.lastEventId = null
    }
    this.setState(ConnectionState.Disconnected)
  }

  /**
   * 立即触发一次重连。会清除当前连接和 pending 中的自动重连定时器。
   */
  reconnectNow(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.nextReconnectAt = null
    this.reconnectAttempt = 0
    this.connect()
  }

  get connected(): boolean {
    return this.es?.readyState === EventSource.OPEN
  }

  getStats(): SseLinkStats {
    return {
      state: this.state,
      reconnectCount: this.reconnectCount,
      lastConnectedAt: this.lastConnectedAt,
      lastEventId: this.lastEventId,
      nextReconnectDelayMs: this.nextReconnectAt
        ? Math.max(0, this.nextReconnectAt - Date.now())
        : null,
      nextReconnectAt: this.nextReconnectAt,
    }
  }

  private setState(state: ConnectionState): void {
    if (this.state === state) return
    this.state = state
    this.options.onStateChange?.(state)
  }

  private scheduleReconnect(waitState = ConnectionState.Reconnecting): void {
    if (this.reconnectTimer) return
    const delay = this.calculateReconnectDelay()
    this.nextReconnectAt = Date.now() + delay
    this.setState(waitState)
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.nextReconnectAt = null
      this.reconnectAttempt += 1
      this.connect()
    }, delay)
  }

  private calculateReconnectDelay(): number {
    const baseIndex = Math.min(this.reconnectAttempt, RECONNECT_BACKOFF_DELAYS_MS.length - 1)
    const baseDelay = RECONNECT_BACKOFF_DELAYS_MS[baseIndex] ?? MAX_RECONNECT_DELAY_MS
    const maxCapped = Math.min(baseDelay, MAX_RECONNECT_DELAY_MS)
    return applyJitter(maxCapped)
  }

  private resetHeartbeatTimer(): void {
    this.clearHeartbeatTimer()
    this.heartbeatTimer = setTimeout(() => {
      // Keep Stale state during the reconnect countdown so UI can show "waiting for data".
      this.scheduleReconnect(ConnectionState.Stale)
    }, this.heartbeatTimeoutMs)
  }

  private clearHeartbeatTimer(): void {
    if (this.heartbeatTimer) {
      clearTimeout(this.heartbeatTimer)
      this.heartbeatTimer = null
    }
  }
}
