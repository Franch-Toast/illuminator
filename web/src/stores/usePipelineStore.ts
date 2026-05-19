import { create } from 'zustand'

export interface ChannelInfo {
  capacity: number
  size: number
  enqueued: number
  dequeued: number
  dropped: number
  flush_injected: number
  backpressure_events: number
  backpressured: boolean
}

export interface PipelineInfo {
  name: string
  running: boolean
  stub: boolean
  batches: number
  records: number
  errors: number
  channel: ChannelInfo
}

interface PipelineState {
  pipelines: PipelineInfo[]
  error: string | null
  loading: boolean
  setPipelines: (p: PipelineInfo[]) => void
  setError: (e: string | null) => void
  setLoading: (l: boolean) => void
}

export const usePipelineStore = create<PipelineState>((set) => ({
  pipelines: [],
  error: null,
  loading: false,
  setPipelines: (pipelines) => set({ pipelines, error: null }),
  setError: (error) => set({ error }),
  setLoading: (loading) => set({ loading }),
}))
