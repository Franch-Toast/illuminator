import { useEffect, useRef } from 'react'
import { api } from '../services/apiClient'
import { usePipelineStore, type PipelineInfo } from '../stores/usePipelineStore'
import { useTimeStore } from '../stores/useTimeStore'

export function usePipelinePolling(intervalMs = 3000) {
  const mode = useTimeStore(s => s.mode)
  const { setPipelines, setError, setLoading } = usePipelineStore()
  const intervalRef = useRef<ReturnType<typeof setInterval>>()

  useEffect(() => {
    if (mode === 'paused') return

    const poll = async () => {
      try {
        const data = await api.pipelines()
        setPipelines((data.pipelines || []).map(p => ({
          name: p.name,
          running: p.running ?? false,
          stub: p.stub,
          batches: p.batches ?? 0,
          records: p.records ?? 0,
          errors: p.errors ?? 0,
          channel: p.channel as PipelineInfo['channel'],
        })))
      } catch (e: unknown) {
        setError(e instanceof Error ? e.message : String(e))
      }
    }

    const initTimer = window.setTimeout(poll, 0)
    intervalRef.current = setInterval(poll, intervalMs)
    return () => {
      clearTimeout(initTimer)
      clearInterval(intervalRef.current)
    }
  }, [mode, intervalMs, setPipelines, setError, setLoading])
}
