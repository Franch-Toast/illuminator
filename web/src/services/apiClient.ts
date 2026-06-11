const BASE = ''

async function get<T = unknown>(path: string, params?: Record<string, string>): Promise<T> {
  const url = params
    ? `${BASE}${path}?${new URLSearchParams(params)}`
    : `${BASE}${path}`
  const res = await fetch(url)
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`)
  return res.json() as Promise<T>
}

async function post<T = unknown>(path: string, body: unknown): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`)
  return res.json() as Promise<T>
}

interface PipelineEntry { name: string; running?: boolean; stub?: boolean; batches?: number; records?: number; errors?: number; channel?: { capacity: number; size: number; enqueued: number; dequeued: number; dropped: number; backpressure_events: number; backpressured: boolean } }
interface ChannelEntry { name: string; pipeline: string; capacity: number; size: number; utilization: number; enqueued: number; dropped: number }

export interface FeatureEntry {
  name: string
  display_name: string
  category: string
  state: 'inactive' | 'active' | 'paused' | 'starting' | 'stopping'
  is_recording: boolean
  batches_processed: number
  records_processed: number
  errors: number
  uptime_ms: number
}

export const api = {
  healthz: () => get<{ status: string; version: string }>('/healthz'),
  pipelines: () => get<{ pipelines: PipelineEntry[] }>('/api/v1/pipelines'),
  channelStats: () => get<{ channels: ChannelEntry[] }>('/api/v1/channel_stats'),
  cpuProfileFlamegraph: () => get('/api/v1/cpu/profile/flamegraph'),
  cpuProfileOffcpu: () => get('/api/v1/cpu/profile/offcpu/snapshot'),
  internalMetrics: () => get('/api/v1/internal_metrics'),
  query: (sql: string) => post<{ rows?: Record<string, unknown>[] }>('/api/v1/query', { query: sql }),

  // Feature management API
  features: () => get<{ features: FeatureEntry[] }>('/api/v1/features'),
  featureStart: (name: string) => post<{ status: string; feature: string; state: string }>(`/api/v1/features/${name}/start`, {}),
  featureStop: (name: string) => post<{ status: string; feature: string; state: string }>(`/api/v1/features/${name}/stop`, {}),
  featurePause: (name: string) => post<{ status: string; feature: string; state: string }>(`/api/v1/features/${name}/pause`, {}),
  featureResume: (name: string) => post<{ status: string; feature: string; state: string }>(`/api/v1/features/${name}/resume`, {}),
  featureCollect: (name: string) => get(`/api/v1/features/${name}/collect`),

  // Recording API
  featureRecordStart: (name: string) =>
    post<{ status: string; feature: string; file: string }>(`/api/v1/features/${name}/record/start`, {}),
  featureRecordStop: (name: string) =>
    post<{ status: string; feature: string; file: string; batches: number; bytes: number }>(`/api/v1/features/${name}/record/stop`, {}),
  featureRecordStatus: (name: string) =>
    get<{ recording: boolean; feature: string; file?: string; bytes_written?: number; batches_written?: number }>(
      `/api/v1/features/${name}/record/status`
    ),
}
