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
interface HistoryPoint { timestamp_ms: number; total_switches: number; avg_latency_us: number }
interface SchedEvent { timestamp_ms: number; event_type: string; prev_pid: number; next_pid: number }
interface WakeupEntry { timestamp_ms: number; waker_pid: number; wakee_pid: number }

export const api = {
  healthz: () => get<{ status: string; version: string }>('/healthz'),
  pipelines: () => get<{ pipelines: PipelineEntry[] }>('/api/v1/pipelines'),
  channelStats: () => get<{ channels: ChannelEntry[] }>('/api/v1/channel_stats'),
  cpuUtilization: () => get('/api/v1/cpu/utilization'),
  cpuProcesses: () => get('/api/v1/cpu/processes'),
  cpuProfileFlamegraph: () => get('/api/v1/cpu/profile/flamegraph'),
  cpuProfileOffcpu: () => get('/api/v1/cpu/profile/offcpu'),
  schedSummary: () => get('/api/v1/cpu/sched/summary'),
  schedHistory: () => get<{ history?: HistoryPoint[] }>('/api/v1/cpu/sched/history'),
  schedEvents: (limit = 500, pid?: number) => {
    const params: Record<string, string> = { limit: String(limit) }
    if (pid !== undefined) params.pid = String(pid)
    return get<{ events?: SchedEvent[] }>('/api/v1/cpu/sched/events', params)
  },
  schedWakeups: () => get<{ wakeups?: WakeupEntry[] }>('/api/v1/cpu/sched/wakeups'),
  internalMetrics: () => get('/api/v1/internal_metrics'),
  query: (sql: string) => post<{ rows?: Record<string, unknown>[] }>('/api/v1/query', { query: sql }),
}
