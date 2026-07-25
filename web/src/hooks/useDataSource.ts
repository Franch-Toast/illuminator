import { useEffect, useRef, useState, useCallback } from 'react'
import type { ConnectionStatus, DataBatch, DataSource } from '../services/dataSource'
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

export interface UseDataSourceOptions<T> {
  feature: string
  transform: (batch: DataBatch) => T | null | undefined
  active?: boolean
  replaySource?: DataSource
  bufferSize?: number
  retryDelayMs?: number
}

export interface UseDataSourceResult<T> {
  data: T[]
  latest: T | null
  error: Error | null
  loading: boolean
  connected: boolean
  retry: () => void
}

const DEFAULT_BUFFER_SIZE = 120
const DEFAULT_RETRY_DELAY_MS = 2000

/**
 * 通用数据订阅 Hook。
 *
 * 封装 DataBus 订阅生命周期、统一 error/loading/connected 状态，并提供类型安全的 transform。
 * 具体 hook（useCpuData 等）只需提供 transform 函数。
 */
export function useDataSource<T>(options: UseDataSourceOptions<T>): UseDataSourceResult<T> {
  const {
    feature,
    transform,
    active = true,
    replaySource,
    bufferSize = DEFAULT_BUFFER_SIZE,
    retryDelayMs = DEFAULT_RETRY_DELAY_MS,
  } = options

  const source = replaySource ?? getDataSource()
  const [data, setData] = useState<T[]>([])
  const [latest, setLatest] = useState<T | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const [loading, setLoading] = useState(true)
  const [connected, setConnected] = useState(() => dataBus.getStatus() === 'connected')
  const bufferRef = useRef<T[]>([])
  const retryTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const transformRef = useRef(transform)

  transformRef.current = transform

  const clearRetryTimer = useCallback(() => {
    if (retryTimerRef.current) {
      clearTimeout(retryTimerRef.current)
      retryTimerRef.current = null
    }
  }, [])

  const retry = useCallback(() => {
    clearRetryTimer()
    setError(null)
    setLoading(true)
    // 触发一次重新订阅：先清空缓冲，依赖 effect 重新挂载时自动恢复
    setData(prev => [...prev])
  }, [clearRetryTimer])

  // 监听连接状态
  useEffect(() => {
    ensureStatusWiring()
    const listener = (s: ConnectionStatus) => setConnected(s === 'connected')
    connectionListeners.add(listener)
    setConnected(dataBus.getStatus() === 'connected')
    return () => { connectionListeners.delete(listener) }
  }, [])

  // 订阅数据
  useEffect(() => {
    if (!active) {
      setLoading(false)
      return
    }

    setLoading(true)

    const unsub = source.subscribe(feature, (batch: DataBatch) => {
      try {
        const transformed = transformRef.current(batch)
        if (transformed == null) return

        bufferRef.current.push(transformed)
        if (bufferRef.current.length > bufferSize) {
          bufferRef.current.shift()
        }

        setData([...bufferRef.current])
        setLatest(transformed)
        setError(null)
        setLoading(false)
        clearRetryTimer()
      } catch (err) {
        const wrapped = err instanceof Error ? err : new Error(String(err))
        setError(wrapped)
        setLoading(false)

        clearRetryTimer()
        retryTimerRef.current = setTimeout(() => {
          setError(null)
        }, retryDelayMs)
      }
    })

    return () => {
      unsub()
      clearRetryTimer()
    }
  }, [active, feature, source, bufferSize, retryDelayMs, clearRetryTimer])

  return { data, latest, error, loading, connected, retry }
}
