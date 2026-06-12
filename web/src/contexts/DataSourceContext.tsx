import { createContext, useContext, useRef, useState, useCallback, useEffect } from 'react'
import type { ReactNode } from 'react'
import type { DataSource, DataBatch, ConnectionStatus } from '../services/dataSource'
import { LiveDataSource } from '../services/liveDataSource'
import { ReplayEngine } from '../services/replayEngine'

type ActiveMode = 'live' | 'replay'

interface DataSourceContextValue {
  mode: ActiveMode
  source: DataSource
  connectionStatus: ConnectionStatus
  switchToLive: () => void
  switchToReplay: (engine: ReplayEngine) => void
}

const DataSourceContext = createContext<DataSourceContextValue | null>(null)

let sharedLiveSource: LiveDataSource | null = null

function getLiveSource(onStatus: (s: ConnectionStatus) => void): LiveDataSource {
  if (!sharedLiveSource) {
    sharedLiveSource = new LiveDataSource({ onConnectionChange: onStatus })
  }
  return sharedLiveSource
}

export function DataSourceProvider({ children }: { children: ReactNode }) {
  const [mode, setMode] = useState<ActiveMode>('live')
  const [connectionStatus, setConnectionStatus] = useState<ConnectionStatus>('disconnected')
  const liveRef = useRef<LiveDataSource>(getLiveSource(setConnectionStatus))
  const replayRef = useRef<ReplayEngine | null>(null)

  const source = mode === 'replay' && replayRef.current
    ? replayRef.current
    : liveRef.current

  const switchToLive = useCallback(() => {
    setMode('live')
  }, [])

  const switchToReplay = useCallback((engine: ReplayEngine) => {
    replayRef.current = engine
    setMode('replay')
  }, [])

  return (
    <DataSourceContext.Provider value={{ mode, source, connectionStatus, switchToLive, switchToReplay }}>
      {children}
    </DataSourceContext.Provider>
  )
}

export function useDataSourceContext(): DataSourceContextValue {
  const ctx = useContext(DataSourceContext)
  if (!ctx) {
    return {
      mode: 'live',
      source: getLiveSource(() => {}),
      connectionStatus: 'disconnected',
      switchToLive: () => {},
      switchToReplay: () => {},
    }
  }
  return ctx
}

export function useFeatureSubscription(feature: string, active = true) {
  const { source } = useDataSourceContext()
  const [latestBatch, setLatestBatch] = useState<DataBatch | null>(null)

  useEffect(() => {
    if (!active) return
    const unsub = source.subscribe(feature, (batch) => {
      setLatestBatch(batch)
    })
    return unsub
  }, [source, feature, active])

  return latestBatch
}
