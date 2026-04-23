import { useState, useEffect, useRef, useCallback } from 'react'

export interface PipelineInfo {
  name: string
  running: boolean
  batches: number
  records: number
  errors: number
}

export function usePipelines(refreshMs = 2000) {
  const [pipelines, setPipelines] = useState<PipelineInfo[]>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<number>()

  const fetch_ = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/pipelines')
      const data = await res.json()
      setPipelines(data.pipelines || [])
      setError(null)
    } catch (e: any) {
      setError(e.message)
    }
  }, [])

  useEffect(() => {
    fetch_()
    intervalRef.current = window.setInterval(fetch_, refreshMs)
    return () => clearInterval(intervalRef.current)
  }, [fetch_, refreshMs])

  return { pipelines, error }
}

export function useHealth() {
  const [health, setHealth] = useState<any>(null)

  useEffect(() => {
    fetch('/healthz')
      .then(r => r.json())
      .then(setHealth)
      .catch(() => setHealth(null))
  }, [])

  return health
}

export async function fetchMetrics(pipeline: string, timeRange?: { start: number; end: number }) {
  const params = new URLSearchParams({ pipeline })
  if (timeRange) {
    params.set('start', String(timeRange.start))
    params.set('end', String(timeRange.end))
  }
  const res = await fetch(`/api/v1/metrics?${params}`)
  return res.json()
}
