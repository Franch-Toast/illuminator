/**
 * useMetricsData — React hook for subscribing to Feature data via DataBus (SSE)
 */

import { useEffect, useRef, useState } from 'react'
import { dataBus } from '../services/dataBus'
import type { DataBatch, DataCallback } from '../services/dataSource'

export function useMetricsData<T = unknown>(feature: string): T | null {
  const [data, setData] = useState<T | null>(null)
  const featureRef = useRef(feature)
  featureRef.current = feature

  useEffect(() => {
    const callback: DataCallback = (batch) => {
      if (batch.modelType === 'time_series' || !batch.modelType) {
        setData(batch.data as T)
      }
    }

    const unsubscribe = dataBus.subscribe(feature, callback)
    return unsubscribe
  }, [feature])

  return data
}

export function useMetricsHistory<T = unknown>(
  feature: string,
  maxItems = 300
): T[] {
  const [history, setHistory] = useState<T[]>([])

  useEffect(() => {
    const callback: DataCallback = (batch) => {
      if (batch.modelType === 'time_series' || !batch.modelType) {
        setHistory((prev) => {
          const next = [...prev, batch.data as T]
          return next.length > maxItems ? next.slice(-maxItems) : next
        })
      }
    }

    const unsubscribe = dataBus.subscribe(feature, callback)
    return unsubscribe
  }, [feature, maxItems])

  return history
}

/**
 * useDataBusConnection — manage DataBus connection lifecycle
 */
export function useDataBusConnection(): { connected: boolean; connect: () => Promise<void>; disconnect: () => void } {
  const [connected, setConnected] = useState(false)

  const connect = async () => {
    try {
      await dataBus.connect()
      setConnected(true)
    } catch {
      setConnected(false)
    }
  }

  const disconnect = () => {
    dataBus.disconnect()
    setConnected(false)
  }

  useEffect(() => {
    const unsub = dataBus.onConnectionChange((s) => {
      setConnected(s === 'connected')
    })
    return unsub
  }, [])

  return { connected, connect, disconnect }
}
