import { useEffect, useRef, useState, useCallback, useMemo } from 'react'
import type { ConnectionStatus } from '../services/dataSource'
import { LiveDataSource } from '../services/liveDataSource'

let globalDataSource: LiveDataSource | null = null
let connectionListeners = new Set<(s: ConnectionStatus) => void>()

export function getDataSource(): LiveDataSource {
  if (!globalDataSource) {
    globalDataSource = new LiveDataSource({
      onConnectionChange: (s) => {
        for (const cb of connectionListeners) cb(s)
      },
    })
  }
  return globalDataSource
}

export function useConnectionStatus(): ConnectionStatus {
  const [status, setStatus] = useState<ConnectionStatus>(
    () => getDataSource().getStatus()
  )

  useEffect(() => {
    connectionListeners.add(setStatus)
    return () => { connectionListeners.delete(setStatus) }
  }, [])

  return status
}

export function usePageActivation(
  category: string,
  features: Array<{ name: string; tier: number }>
) {
  const activatedRef = useRef(new Set<string>())

  const activateFeatures = useCallback(async () => {
    const { api } = await import('../services/apiClient')

    const tierFeatures = features.filter(f => f.tier <= 2)
    for (const f of tierFeatures) {
      if (activatedRef.current.has(f.name)) continue
      try {
        await api.featureStart(f.name)
        activatedRef.current.add(f.name)
      } catch { /* already running or failed — both ok */ }
    }
  }, [features])

  useEffect(() => {
    activateFeatures()
  }, [activateFeatures])

  return {
    activatedFeatures: activatedRef.current,
    manualStart: async (name: string) => {
      const { api } = await import('../services/apiClient')
      await api.featureStart(name)
      activatedRef.current.add(name)
    },
    manualStop: async (name: string) => {
      const { api } = await import('../services/apiClient')
      await api.featureStop(name)
      activatedRef.current.delete(name)
    },
  }
}

export interface BudgetInfo {
  usage: { rss_bytes: number; cpu_pct: number; active_features: number; ebpf_probes: number }
  limits: { max_memory_bytes: number; max_cpu_pct: number; max_ebpf_probes: number }
  exceeded: boolean
}

export function useResourceBudget(pollMs = 5000): BudgetInfo | null {
  const [budget, setBudget] = useState<BudgetInfo | null>(null)

  useEffect(() => {
    const fetchBudget = async () => {
      try {
        const { api } = await import('../services/apiClient')
        const resp = await (api as unknown as { budget: () => Promise<BudgetInfo> }).budget()
        setBudget(resp)
      } catch { /* ignore */ }
    }
    fetchBudget()
    const timer = setInterval(fetchBudget, pollMs)
    return () => clearInterval(timer)
  }, [pollMs])

  return budget
}

export function useFeaturesByCategory(category: string) {
  const [features, setFeatures] = useState<Array<{ name: string; tier: number; state: string }>>([])

  useEffect(() => {
    const fetch = async () => {
      const { api } = await import('../services/apiClient')
      const resp = await api.features()
      const filtered = (resp.features || [])
        .filter(f => f.category === category)
        .map(f => ({
          name: f.name,
          tier: (f as unknown as { tier: number }).tier || 1,
          state: f.state,
        }))
      setFeatures(filtered)
    }
    fetch()
  }, [category])

  return useMemo(() => features, [features])
}
