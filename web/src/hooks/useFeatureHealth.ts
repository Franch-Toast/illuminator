import { useEffect, useRef, useState } from 'react'
import { getDataSource } from './useDataSource'
import { api } from '../services/apiClient'

export type FeatureHealthStatus = 'active' | 'degraded' | 'unavailable' | 'error'

export interface FeatureHealthInfo {
  status: FeatureHealthStatus
  state: string
  uptime_ms: number
  batches_processed: number
  records_processed: number
  errors: number
  lastUpdatedAt: number
  // BPF self-observability counters (cumulative) and computed overflow rate
  bpf_total_events: number
  bpf_buffer_full: number
  bpf_dropped: number
  bpf_filtered: number
  buffer_full_rate: number // percent (0..100), computed from deltas
}

const DEFAULT_HEALTH: FeatureHealthInfo = {
  status: 'active',
  state: 'unknown',
  uptime_ms: 0,
  batches_processed: 0,
  records_processed: 0,
  errors: 0,
  lastUpdatedAt: 0,
  bpf_total_events: 0,
  bpf_buffer_full: 0,
  bpf_dropped: 0,
  bpf_filtered: 0,
  buffer_full_rate: 0,
}

const DEGRADED_MS = 5000
const UNAVAILABLE_MS = 15000
const STATS_POLL_MS = 5000

/**
 * Monitors a feature's health by combining:
 *  - Data-flow liveness (timestamps of incoming SSE batches)
 *  - Periodic backend stats (state, uptime, batches/records processed, errors)
 *
 * Returns `error` when the backend reports errors or an error state, which takes
 * precedence over data-flow based statuses.
 */
export function useFeatureHealth(featureName: string): FeatureHealthInfo {
  const [health, setHealth] = useState<FeatureHealthInfo>(DEFAULT_HEALTH)
  const lastSeenRef = useRef(0)
  const statsRef = useRef<FeatureHealthInfo>(DEFAULT_HEALTH)
  const prevBpfRef = useRef<{ total: number; full: number }>({ total: 0, full: 0 })

  useEffect(() => {
    lastSeenRef.current = Date.now()
    let degradedTimer: ReturnType<typeof setTimeout>
    let unavailableTimer: ReturnType<typeof setTimeout>

    const computeHealth = (): FeatureHealthInfo => {
      const stats = statsRef.current
      const now = Date.now()
      const elapsed = now - lastSeenRef.current

      // Backend-reported errors take highest priority.
      if (stats.errors > 0 || stats.state === 'error') {
        return { ...stats, status: 'error', lastUpdatedAt: now }
      }
      if (elapsed > UNAVAILABLE_MS) {
        return { ...stats, status: 'unavailable', lastUpdatedAt: now }
      }
      if (elapsed > DEGRADED_MS) {
        return { ...stats, status: 'degraded', lastUpdatedAt: now }
      }
      return { ...stats, status: 'active', lastUpdatedAt: now }
    }

    const resetTimers = () => {
      lastSeenRef.current = Date.now()
      clearTimeout(degradedTimer)
      clearTimeout(unavailableTimer)
      setHealth(computeHealth())
      degradedTimer = setTimeout(() => setHealth(computeHealth()), DEGRADED_MS)
      unavailableTimer = setTimeout(() => setHealth(computeHealth()), UNAVAILABLE_MS)
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

  useEffect(() => {
    let cancelled = false

    const fetchStats = async () => {
      try {
        const stats = await api.featureStats(featureName)
        if (cancelled) return

        const totalEvents = Number(stats.bpf_total_events ?? 0)
        const bufferFull = Number(stats.bpf_buffer_full ?? 0)
        const dropped = Number(stats.bpf_dropped ?? 0)
        const filtered = Number(stats.bpf_filtered ?? 0)

        const totalDelta = Math.max(0, totalEvents - prevBpfRef.current.total)
        const fullDelta = Math.max(0, bufferFull - prevBpfRef.current.full)
        const bufferFullRate = totalDelta > 0 ? (fullDelta / totalDelta) * 100 : 0

        prevBpfRef.current = { total: totalEvents, full: bufferFull }

        statsRef.current = {
          state: String(stats.state ?? 'unknown'),
          uptime_ms: Number(stats.uptime_ms ?? 0),
          batches_processed: Number(stats.batches_processed ?? 0),
          records_processed: Number(stats.records_processed ?? 0),
          errors: Number(stats.errors ?? 0),
          status: 'active',
          lastUpdatedAt: Date.now(),
          bpf_total_events: totalEvents,
          bpf_buffer_full: bufferFull,
          bpf_dropped: dropped,
          bpf_filtered: filtered,
          buffer_full_rate: bufferFullRate,
        }
      } catch {
        // Keep existing stats on error; data-flow timers still report degraded/unavailable.
      }
    }

    fetchStats()
    const timer = setInterval(fetchStats, STATS_POLL_MS)
    return () => {
      cancelled = true
      clearInterval(timer)
    }
  }, [featureName])

  return health
}
