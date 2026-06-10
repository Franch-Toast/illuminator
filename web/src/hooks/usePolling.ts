import { useEffect, useRef } from 'react'
import { useTimeStore } from '../stores/useTimeStore'

export function usePolling(callback: () => void, intervalMs: number) {
  const mode = useTimeStore(s => s.mode)
  const callbackRef = useRef(callback)

  useEffect(() => {
    callbackRef.current = callback
  })

  useEffect(() => {
    if (mode === 'paused') return

    const initTimer = window.setTimeout(() => callbackRef.current(), 0)
    const intervalId = setInterval(() => callbackRef.current(), intervalMs)
    return () => {
      clearTimeout(initTimer)
      clearInterval(intervalId)
    }
  }, [mode, intervalMs])
}
