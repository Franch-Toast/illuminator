// ============================================================================
// Illuminator Web Hooks — API 数据获取层
// ============================================================================
//
// 本文件封装了与 Illuminator 后端的 HTTP 通信逻辑。
//
// 三个自定义 Hook：
// ==================
// 1. usePipelines(refreshMs)
//    - 定期 GET /api/v1/pipelines 获取所有管道的运行状态
//    - 返回 PipelineInfo 数组和错误信息
//    - 自动在组件卸载时清除定时器
//
// 2. useHealth()
//    - 一次性 GET /healthz 检查后端服务可用性
//    - 返回健康检查响应的 JSON 对象
//
// 3. fetchMetrics(pipeline, timeRange?)
//    - 通用指标查询函数，GET /api/v1/internal_metrics?pipeline=xxx&start=...&end=...
//    - 用于按管道名称和时间范围获取历史指标数据
// ============================================================================

import { useState, useEffect, useRef, useCallback } from 'react'

// 管道运行信息（对应后端 Pipeline 的统计字段）
export interface PipelineInfo {
  name: string
  running: boolean
  batches: number
  records: number
  errors: number
  channel?: ChannelInfo
}

// 异步通道统计（对应后端 AsyncChannel 的 Stats）
export interface ChannelInfo {
  capacity: number
  size: number
  enqueued: number
  dequeued: number
  dropped: number
  backpressure_events: number
  backpressured: boolean
}

// ---- usePipelines: 定期获取管道状态 ----
// refreshMs: 轮询间隔（毫秒），默认 2000ms
export function usePipelines(refreshMs = 2000) {
  const [pipelines, setPipelines] = useState<PipelineInfo[]>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<number>()

  // 实际的数据获取函数
  const fetch_ = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/pipelines')
      if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`)
      const data = await res.json()
      setPipelines(data.pipelines || [])
      setError(null)
    } catch (e: any) {
      setError(e.message)
    }
  }, [])

  // 初次加载 + 定期轮询
  useEffect(() => {
    fetch_()
    intervalRef.current = window.setInterval(fetch_, refreshMs)
    return () => clearInterval(intervalRef.current)
  }, [fetch_, refreshMs])

  return { pipelines, error }
}

// ---- useHealth: 检查后端可用性 ----
export function useHealth() {
  const [health, setHealth] = useState<any>(null)

  useEffect(() => {
    fetch('/healthz')
      .then(r => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json()
      })
      .then(setHealth)
      .catch(() => setHealth(null))
  }, [])

  return health
}

// ---- fetchMetrics: 通用指标查询 ----
// pipeline: 管道名称
// timeRange: 可选的时间范围 { start, end }（纳秒时间戳）
export async function fetchMetrics(pipeline: string, timeRange?: { start: number; end: number }) {
  const params = new URLSearchParams({ pipeline })
  if (timeRange) {
    params.set('start', String(timeRange.start))
    params.set('end', String(timeRange.end))
  }
  const res = await fetch(`/api/v1/internal_metrics?${params}`)
  if (!res.ok) {
    throw new Error(`HTTP ${res.status}: ${res.statusText}`)
  }
  return res.json()
}
