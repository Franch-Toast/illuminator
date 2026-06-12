export interface DataBatch {
  feature: string
  timestamp: number
  data: unknown
}

export type DataCallback = (batch: DataBatch) => void

export interface DataSource {
  subscribe(feature: string, cb: DataCallback): () => void
  getLatest(feature: string): DataBatch | null
  getAvailableFeatures(): string[]
  destroy(): void
}

export type ConnectionStatus = 'connected' | 'connecting' | 'disconnected'

export interface DataSourceEvents {
  onConnectionChange?: (status: ConnectionStatus) => void
}
