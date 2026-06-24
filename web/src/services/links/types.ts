import type { DataBatch, ConnectionStatus } from '../dataSource'

export type DataHandler = (batch: DataBatch) => void
export type StatusHandler = (status: ConnectionStatus) => void

export interface DataLink {
  readonly name: string
  connect(): void
  disconnect(): void
  subscribe(feature: string): void
  unsubscribe(feature: string): void
  isConnected(): boolean
}
