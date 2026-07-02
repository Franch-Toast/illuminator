/**
 * SSE Link — EventSource 封装
 *
 * 建立 SSE 连接并将事件分发给回调。自动重连、心跳检测。
 */

export interface SseLinkOptions {
  url: string
  onData: (feature: string, data: unknown) => void
  onFrame: (frame: SseFrame) => void
  onError?: (error: Event) => void
  onOpen?: () => void
}

export interface SseFrame {
  feature: string
  seq: number
  frame_idx: number
  frame_total: number
  payload: string
}

export class SseLink {
  private es: EventSource | null = null
  private options: SseLinkOptions
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null
  private reconnectDelay = 1000

  constructor(options: SseLinkOptions) {
    this.options = options
  }

  connect(): void {
    this.disconnect()

    this.es = new EventSource(this.options.url)

    this.es.onopen = () => {
      this.reconnectDelay = 1000
      this.options.onOpen?.()
    }

    this.es.onerror = (e) => {
      this.options.onError?.(e)
      this.scheduleReconnect()
    }

    this.es.addEventListener('data', (event: MessageEvent) => {
      try {
        const parsed = JSON.parse(event.data)
        const feature = parsed.feature || ''
        this.options.onData(feature, parsed)
      } catch {
        // Ignore parse errors
      }
    })

    this.es.addEventListener('frame', (event: MessageEvent) => {
      try {
        const frame: SseFrame = JSON.parse(event.data)
        this.options.onFrame(frame)
      } catch {
        // Ignore parse errors
      }
    })
  }

  disconnect(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    if (this.es) {
      this.es.close()
      this.es = null
    }
  }

  get connected(): boolean {
    return this.es?.readyState === EventSource.OPEN
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) return
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      this.reconnectDelay = Math.min(this.reconnectDelay * 2, 30000)
      this.connect()
    }, this.reconnectDelay)
  }
}
