type MessageHandler = (data: unknown) => void

interface Subscription {
  handlers: Set<MessageHandler>
  ws: WebSocket | null
  retries: number
  reconnectTimer: ReturnType<typeof setTimeout> | null
}

class WsManagerImpl {
  private subs = new Map<string, Subscription>()
  private statusListeners = new Set<() => void>()

  subscribe(pipelineKey: string, handler: MessageHandler): () => void {
    let sub = this.subs.get(pipelineKey)
    if (!sub) {
      sub = { handlers: new Set(), ws: null, retries: 0, reconnectTimer: null }
      this.subs.set(pipelineKey, sub)
    }
    sub.handlers.add(handler)

    if (!sub.ws || sub.ws.readyState > WebSocket.OPEN) {
      this.connect(pipelineKey)
    }

    return () => {
      sub!.handlers.delete(handler)
      if (sub!.handlers.size === 0) {
        this.disconnect(pipelineKey)
      }
    }
  }

  getStatus(pipelineKey: string): 'connecting' | 'connected' | 'disconnected' {
    const sub = this.subs.get(pipelineKey)
    if (!sub?.ws) return 'disconnected'
    if (sub.ws.readyState === WebSocket.OPEN) return 'connected'
    if (sub.ws.readyState === WebSocket.CONNECTING) return 'connecting'
    return 'disconnected'
  }

  onStatusChange(callback: () => void): () => void {
    this.statusListeners.add(callback)
    return () => this.statusListeners.delete(callback)
  }

  private connect(key: string) {
    const sub = this.subs.get(key)
    if (!sub) return

    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const url = `${protocol}//${window.location.host}/ws/${key}`

    try {
      const ws = new WebSocket(url)
      sub.ws = ws

      ws.onopen = () => {
        sub.retries = 0
        this.notifyStatus()
      }

      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data)
          for (const handler of sub.handlers) handler(data)
        } catch { /* ignore parse errors */ }
      }

      ws.onclose = () => {
        sub.ws = null
        this.notifyStatus()
        if (sub.handlers.size > 0) {
          const delay = Math.min(1000 * Math.pow(2, sub.retries), 30000)
          sub.retries++
          sub.reconnectTimer = setTimeout(() => this.connect(key), delay)
        }
      }

      ws.onerror = () => ws.close()
      this.notifyStatus()
    } catch {
      const delay = Math.min(1000 * Math.pow(2, sub.retries), 30000)
      sub.retries++
      sub.reconnectTimer = setTimeout(() => this.connect(key), delay)
    }
  }

  private disconnect(key: string) {
    const sub = this.subs.get(key)
    if (!sub) return
    if (sub.reconnectTimer) clearTimeout(sub.reconnectTimer)
    sub.ws?.close()
    sub.ws = null
    this.subs.delete(key)
    this.notifyStatus()
  }

  private notifyStatus() {
    for (const cb of this.statusListeners) cb()
  }
}

export const wsManager = new WsManagerImpl()
