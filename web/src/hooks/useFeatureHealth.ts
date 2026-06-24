import { useEffect, useRef, useState } from 'react'
import { getDataSource } from './useDataSource'

export type FeatureHealthStatus = 'active' | 'degraded' | 'unavailable'

/**
 * Monitors data availability for a feature. Returns 'active' when data
 * arrives within 5s, 'degraded' after a 5s gap, and 'unavailable' after 15s.
 * Since features are always-on (Tier 1-2 started by daemon), this primarily
 * detects backend outages or eBPF load failures rather than "not started" state.
 */
export function useFeatureHealth(featureName: string): FeatureHealthStatus {
  const [health, setHealth] = useState<FeatureHealthStatus>('active')
  const lastSeenRef = useRef(0)

  useEffect(() => {
    lastSeenRef.current = Date.now()
    let degradedTimer: ReturnType<typeof setTimeout>
    let unavailableTimer: ReturnType<typeof setTimeout>

    const resetTimers = () => {
      lastSeenRef.current = Date.now()
      setHealth('active')
      clearTimeout(degradedTimer)
      clearTimeout(unavailableTimer)
      degradedTimer = setTimeout(() => setHealth('degraded'), 5000)
      unavailableTimer = setTimeout(() => setHealth('unavailable'), 15000)
    }

    resetTimers()

    const source = getDataSource()
    const unsub = source.subscribe(featureName, () => {
      resetTimers()
    })

    return () => {
      unsub()
      clearTimeout(degradedTimer)
      clearTimeout(unavailableTimer)
    }
  }, [featureName])

  return health
}
