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

export interface FeatureDescriptor {
  name: string
  version: string
  display_name: string
  category: string
  tier: number
  state: string
  data_model: string
  capabilities: string[]
  parameters: Array<{
    name: string
    type: string
    description: string
    default_value?: string
    required?: boolean
  }>
}

export interface FeatureEntry {
  name: string
  display_name: string
  category: string
  tier: number
  state: 'inactive' | 'active' | 'paused' | 'starting' | 'stopping'
  is_recording: boolean
  batches_processed: number
  records_processed: number
  errors: number
  uptime_ms: number
}

export interface BudgetResponse {
  usage: { rss_bytes: number; cpu_pct: number; active_features: number; ebpf_probes: number }
  limits: { max_memory_bytes: number; max_cpu_pct: number; max_ebpf_probes: number }
  exceeded: boolean
}

export const api = {
  healthz: () => get<{ status: string; version: string }>('/healthz'),
  pipelines: () => get<{ pipelines: PipelineEntry[] }>('/api/v1/pipelines'),
  channelStats: () => get<{ channels: ChannelEntry[] }>('/api/v1/channel_stats'),
  internalMetrics: () => get('/api/v1/internal_metrics'),
  query: (sql: string) => post<{ rows?: Record<string, unknown>[] }>('/api/v1/query', { query: sql }),

  // ─── v2 Feature API (RFC v3 FeatureBus) ──────────────────────────────
  features: () => get<{ features: FeatureDescriptor[] }>('/api/v2/features'),
  getConfigSchema: (name: string) => get<Record<string, unknown>>(`/api/v2/features/${name}/config/schema`),
  getFeatureConfig: (name: string) => get<Record<string, unknown>>(`/api/v2/features/${name}/config`),
  setFeatureConfig: (name: string, config: Record<string, unknown>) =>
    post<{ status: string }>(`/api/v2/features/${name}/config`, config),

  featureStart: (name: string, config?: Record<string, unknown>) => post<{ status: string }>(`/api/v2/features/${name}/start`, config ?? {}),
  featureStop: (name: string) => post<{ status: string }>(`/api/v2/features/${name}/stop`, {}),
  featurePause: (name: string) => post<{ status: string }>(`/api/v2/features/${name}/pause`, {}),
  featureResume: (name: string) => post<{ status: string }>(`/api/v2/features/${name}/resume`, {}),
  featureStats: (name: string) => get<Record<string, unknown>>(`/api/v2/features/${name}/stats`),

  // ─── Recording API ────────────────────────────────────────────────────
  startRecording: (name: string, outputDir: string) =>
    post<{ status: string }>(`/api/v1/features/${name}/record/start`, { output_dir: outputDir }),
  stopRecording: (name: string) =>
    post<{ status: string }>(`/api/v1/features/${name}/record/stop`, {}),
  recordingStatus: (name: string) =>
    get<{ recording: boolean; file?: string }>(`/api/v1/features/${name}/record/status`),

  // ─── Resource Budget ──────────────────────────────────────────────────
  budget: () => get<BudgetResponse>('/api/v1/budget'),

  // ─── Plugin Hot-reload ────────────────────────────────────────────────
  pluginsReload: () => post<{ status: string; loaded: number; plugins: string[] }>('/api/v1/plugins/reload', {}),
}
