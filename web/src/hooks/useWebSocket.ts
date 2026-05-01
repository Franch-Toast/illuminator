import { useState, useEffect, useRef, useCallback } from 'react'

export interface WebSocketOptions {
  url?: string
  reconnectMs?: number
  onMessage?: (data: any) => void
}

export function useWebSocket(opts: WebSocketOptions = {}) {
  const {
    url = `ws://${window.location.host}/ws/v1/cpu/stream`,
    reconnectMs = 3000,
  } = opts

  const [connected, setConnected] = useState(false)
  const [lastMessage, setLastMessage] = useState<any>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const reconnectTimer = useRef<number>()
  const onMessageRef = useRef(opts.onMessage)
  onMessageRef.current = opts.onMessage

  const connect = useCallback(() => {
    try {
      const ws = new WebSocket(url)
      wsRef.current = ws

      ws.onopen = () => {
        setConnected(true)
      }

      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data)
          setLastMessage(data)
          onMessageRef.current?.(data)
        } catch {}
      }

      ws.onclose = () => {
        setConnected(false)
        wsRef.current = null
        reconnectTimer.current = window.setTimeout(connect, reconnectMs)
      }

      ws.onerror = () => {
        ws.close()
      }
    } catch {
      reconnectTimer.current = window.setTimeout(connect, reconnectMs)
    }
  }, [url, reconnectMs])

  useEffect(() => {
    connect()
    return () => {
      clearTimeout(reconnectTimer.current)
      wsRef.current?.close()
    }
  }, [connect])

  const send = useCallback((data: any) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(data))
    }
  }, [])

  return { connected, lastMessage, send }
}
