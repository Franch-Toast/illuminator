import { useEffect, useState } from 'react'
import type { ConnectionStatus } from '../services/dataSource'
import { dataBus, DataBus } from '../services/dataBus'

const connectionListeners = new Set<(s: ConnectionStatus) => void>()

let unsubStatus: (() => void) | null = null

function ensureStatusWiring() {
  if (unsubStatus) return
  unsubStatus = dataBus.onConnectionChange((s) => {
    for (const cb of connectionListeners) cb(s)
  })
}

export function getDataSource(): DataBus {
  ensureStatusWiring()
  return dataBus
}

export function useConnectionStatus(): ConnectionStatus {
  const [status, setStatus] = useState<ConnectionStatus>(
    () => dataBus.getStatus()
  )

  useEffect(() => {
    ensureStatusWiring()
    connectionListeners.add(setStatus)
    return () => { connectionListeners.delete(setStatus) }
  }, [])

  return status
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
