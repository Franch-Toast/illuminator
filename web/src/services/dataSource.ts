export type DataModelType = 'time_series' | 'profile' | 'trace' | 'log' | 'generic'

export interface DataBatch {
  feature: string
  timestamp: number
  modelType?: DataModelType
  data: unknown
}

export type DataCallback = (batch: DataBatch) => void

export interface DataSource {
  subscribe(feature: string, cb: DataCallback): () => void
  getLatest(feature: string): DataBatch | null
  getAvailableFeatures(): string[]
  destroy(): void
}

export type ConnectionStatus = 'connected' | 'connecting' | 'reconnecting' | 'stale' | 'error' | 'disconnected'

export interface DataSourceEvents {
  onConnectionChange?: (status: ConnectionStatus) => void
}
