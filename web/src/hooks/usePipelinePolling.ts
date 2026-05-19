import { useEffect, useRef } from 'react'
import { api } from '../services/apiClient'
import { usePipelineStore } from '../stores/usePipelineStore'
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
        setPipelines(data.pipelines || [])
      } catch (e: any) {
        setError(e.message)
      }
    }

    poll()
    intervalRef.current = setInterval(poll, intervalMs)
    return () => clearInterval(intervalRef.current)
  }, [mode, intervalMs, setPipelines, setError, setLoading])
}
