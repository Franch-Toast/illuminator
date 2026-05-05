// ============================================================================
// Illuminator Web Hooks — WebSocket 实时数据推送
// ============================================================================
//
// 本 Hook 封装了与后端 WebSocket 端点的连接管理，支持：
// - 自动连接和重连（指数退避）
// - JSON 消息解析
// - 连接状态追踪
// - 组件卸载时自动清理
//
// 使用示例：
//   const { connected, lastMessage, send } = useWebSocket({
//     url: 'ws://localhost:9527/ws/v1/cpu/stream',
//     onMessage: (data) => console.log('Received:', data),
//   })
//
// 典型应用场景：
// ==============
// - 实时推送 CPU 采样数据到火焰图
// - 流式推送调度事件到时间线视图
// - 前端状态与后端管道状态的实时同步
// ============================================================================

import { useState, useEffect, useRef, useCallback } from 'react'

export interface WebSocketOptions {
  url?: string              // WebSocket 服务地址（默认根据当前 host 自动构造）
  reconnectMs?: number      // 重连间隔（默认 3000ms）
  onMessage?: (data: any) => void  // 收到消息时的回调
}

export function useWebSocket(opts: WebSocketOptions = {}) {
  const {
    // 默认连接到同主机的 /ws/v1/cpu/stream 端点
    url = `ws://${window.location.host}/ws/v1/cpu/stream`,
    reconnectMs = 3000,
  } = opts

  const [connected, setConnected] = useState(false)        // 连接状态
  const [lastMessage, setLastMessage] = useState<any>(null) // 最新收到的消息
  const wsRef = useRef<WebSocket | null>(null)              // WebSocket 实例引用
  const reconnectTimer = useRef<number>()                   // 重连定时器
  const onMessageRef = useRef(opts.onMessage)               // 回调函数引用（避免闭包过期）
  onMessageRef.current = opts.onMessage

  // ---- 连接函数 ----
  const connect = useCallback(() => {
    try {
      const ws = new WebSocket(url)
      wsRef.current = ws

      // 连接成功
      ws.onopen = () => {
        setConnected(true)
      }

      // 收到消息：解析 JSON 并回调
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data)
          setLastMessage(data)
          onMessageRef.current?.(data)
        } catch {}  // 非法 JSON 忽略
      }

      // 连接关闭：标记断开，启动重连定时器
      ws.onclose = () => {
        setConnected(false)
        wsRef.current = null
        reconnectTimer.current = window.setTimeout(connect, reconnectMs)
      }

      // 连接错误：主动关闭触发 onclose 重连逻辑
      ws.onerror = () => {
        ws.close()
      }
    } catch {
      // 创建 WebSocket 失败：延时重试
      reconnectTimer.current = window.setTimeout(connect, reconnectMs)
    }
  }, [url, reconnectMs])

  // ---- 生命周期管理 ----
  useEffect(() => {
    connect()  // 初次连接
    return () => {
      // 组件卸载时清理
      clearTimeout(reconnectTimer.current)
      wsRef.current?.close()
    }
  }, [connect])

  // ---- 发送消息 ----
  const send = useCallback((data: any) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(data))
    }
  }, [])

  return { connected, lastMessage, send }
}
