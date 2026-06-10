import { useState, useEffect, useRef, useCallback } from 'react'

export type ConnectionState = 'connecting' | 'connected' | 'reconnecting' | 'disconnected'

export interface WebSocketOptions {
  pipelineKey: string
  onMessage?: (data: unknown) => void
  enabled?: boolean
}

export function useWebSocket(opts: WebSocketOptions) {
  const { pipelineKey, enabled = true } = opts

  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected')
  const [lastMessage, setLastMessage] = useState<unknown>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const reconnectTimer = useRef<number>()
  const retriesRef = useRef(0)
  const onMessageRef = useRef(opts.onMessage)
  const enabledRef = useRef(enabled)

  useEffect(() => {
    onMessageRef.current = opts.onMessage
    enabledRef.current = enabled
  })

  useEffect(() => {
    if (!enabled) return

    function connect() {
      if (!enabledRef.current) return

      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      const url = `${protocol}//${window.location.host}/ws/${pipelineKey}`

      setConnectionState(retriesRef.current > 0 ? 'reconnecting' : 'connecting')

      try {
        const ws = new WebSocket(url)
        wsRef.current = ws

        ws.onopen = () => {
          retriesRef.current = 0
          setConnectionState('connected')
        }

        ws.onmessage = (event) => {
          try {
            const data = JSON.parse(event.data)
            setLastMessage(data)
            onMessageRef.current?.(data)
          } catch { /* malformed JSON — skip frame */ }
        }

        ws.onclose = () => {
          setConnectionState('disconnected')
          wsRef.current = null
          if (enabledRef.current) {
            const delay = Math.min(1000 * Math.pow(2, retriesRef.current), 30000)
            retriesRef.current++
            reconnectTimer.current = window.setTimeout(connect, delay)
          }
        }

        ws.onerror = () => ws.close()
      } catch {
        const delay = Math.min(1000 * Math.pow(2, retriesRef.current), 30000)
        retriesRef.current++
        reconnectTimer.current = window.setTimeout(connect, delay)
      }
    }

    retriesRef.current = 0
    connect()

    return () => {
      clearTimeout(reconnectTimer.current)
      wsRef.current?.close()
      wsRef.current = null
    }
  }, [pipelineKey, enabled])

  const send = useCallback((data: unknown) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(typeof data === 'string' ? data : JSON.stringify(data))
    }
  }, [])

  return { connectionState, lastMessage, send, connected: connectionState === 'connected' }
}
