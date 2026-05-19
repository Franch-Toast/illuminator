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

export const api = {
  healthz: () => get<{ status: string; version: string }>('/healthz'),
  pipelines: () => get<{ pipelines: any[] }>('/api/v1/pipelines'),
  channelStats: () => get<{ channels: any[] }>('/api/v1/channel_stats'),
  cpuUtilization: () => get('/api/v1/cpu/utilization'),
  cpuProcesses: () => get('/api/v1/cpu/processes'),
  cpuProfileFlamegraph: () => get('/api/v1/cpu/profile/flamegraph'),
  cpuProfileOffcpu: () => get('/api/v1/cpu/profile/offcpu'),
  schedSummary: () => get('/api/v1/cpu/sched/summary'),
  schedHistory: () => get<{ history?: any[] }>('/api/v1/cpu/sched/history'),
  schedEvents: (limit = 500, pid?: number) => {
    const params: Record<string, string> = { limit: String(limit) }
    if (pid !== undefined) params.pid = String(pid)
    return get<{ events?: any[] }>('/api/v1/cpu/sched/events', params)
  },
  schedWakeups: () => get<{ wakeups?: any[] }>('/api/v1/cpu/sched/wakeups'),
  internalMetrics: () => get('/api/v1/internal_metrics'),
  query: (sql: string) => post<{ rows?: any[] }>('/api/v1/query', { query: sql }),
}
