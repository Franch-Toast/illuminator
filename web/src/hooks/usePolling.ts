import { useEffect, useRef } from 'react'
import { useTimeStore } from '../stores/useTimeStore'

export function usePolling(callback: () => void, intervalMs: number) {
  const mode = useTimeStore(s => s.mode)
  const callbackRef = useRef(callback)
  callbackRef.current = callback
  const intervalRef = useRef<ReturnType<typeof setInterval>>()

  useEffect(() => {
    if (mode === 'paused') return

    callbackRef.current()
    intervalRef.current = setInterval(() => callbackRef.current(), intervalMs)
    return () => clearInterval(intervalRef.current)
  }, [mode, intervalMs])
}
