import React, { useState, useEffect, useRef, useCallback, useMemo } from 'react'
import {
  ResponsiveContainer,
  BarChart,
  Bar,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
} from 'recharts'
import { useWebSocket, ConnectionState } from '../hooks/useWebSocket'

interface SchedSummary {
  pid: number
  comm: string
  switch_count: number
  avg_runqueue_latency_us: number
  max_runqueue_latency_us: number
  migrate_count: number
}

interface HistoryPoint {
  timestamp_ms: number
  total_switches: number
  avg_latency_us: number
  max_latency_ns: number
  total_migrations: number
  process_count: number
}

interface SchedEvent {
  timestamp_ms: number
  event_type: string
  prev_pid: number
  next_pid: number
  cpu: number
  latency_ns: number
  prev_comm: string
  next_comm: string
}

interface WakeupEntry {
  timestamp_ms: number
  waker_pid: number
  wakee_pid: number
  waker_comm: string
  wakee_comm: string
}

interface LatencyBucket { range: string; count: number }

const card: React.CSSProperties = {
  background: '#1a1d23', borderRadius: 8, padding: 20,
  border: '1px solid #2a2d35',
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '6px 14px', fontSize: 13, cursor: 'pointer',
}

const tabBtnStyle = (active: boolean): React.CSSProperties => ({
  ...btnStyle,
  background: active ? '#1d4ed8' : '#374151',
  fontSize: 12, padding: '6px 12px',
})

function WsIndicator({ state }: { state: ConnectionState }) {
  const color = state === 'connected' ? '#4ade80' :
                state === 'connecting' || state === 'reconnecting' ? '#f59e0b' : '#6b7280'
  const label = state === 'connected' ? 'WS Live' :
                state === 'reconnecting' ? 'WS Reconnecting' : 'HTTP Polling'
  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 11, color: '#888' }}>
      <span style={{ width: 8, height: 8, borderRadius: '50%', background: color, display: 'inline-block' }} />
      {label}
    </span>
  )
}

function parseSchedData(data: any): SchedSummary[] {
  if (!data?.records) return []
  const results: SchedSummary[] = []
  for (const rec of data.records) {
    const labels = rec.labels || {}
    const fields = rec.fields || {}
    if (fields.switch_count !== undefined) {
      const pid = fields.pid || parseInt(labels.pid || '0')
      const switchCount = fields.switch_count || 0
      const totalLatNs = fields.total_runqueue_latency_ns || 0
      const maxLatNs = fields.max_runqueue_latency_ns || 0
      const avgLatUs = switchCount > 0 ? totalLatNs / switchCount / 1000 : 0
      const maxLatUs = maxLatNs / 1000
      results.push({
        pid,
        comm: labels.comm || fields.comm || `pid:${pid}`,
        switch_count: switchCount,
        avg_runqueue_latency_us: avgLatUs,
        max_runqueue_latency_us: maxLatUs,
        migrate_count: fields.migrate_count || 0,
      })
    }
  }
  return results.sort((a, b) => b.switch_count - a.switch_count)
}

function buildLatencyHistogram(summaries: SchedSummary[]): LatencyBucket[] {
  const buckets = [
    { range: '<10us', max: 10, count: 0 },
    { range: '10-50us', max: 50, count: 0 },
    { range: '50-100us', max: 100, count: 0 },
    { range: '100-500us', max: 500, count: 0 },
    { range: '500us-1ms', max: 1000, count: 0 },
    { range: '1-5ms', max: 5000, count: 0 },
    { range: '>5ms', max: Infinity, count: 0 },
  ]
  for (const s of summaries) {
    const lat = s.avg_runqueue_latency_us
    for (const b of buckets) {
      if (lat < b.max) { b.count++; break }
    }
  }
  return buckets.map(b => ({ range: b.range, count: b.count }))
}

type TabKey = 'overview' | 'timeseries' | 'migrations' | 'wakeups'

export default function Timeline() {
  const [summaries, setSummaries] = useState<SchedSummary[]>([])
  const [error, setError] = useState<string | null>(null)
  const [stub, setStub] = useState(false)
  const [sortBy, setSortBy] = useState<'switches' | 'latency' | 'migrations'>('switches')
  const [activeTab, setActiveTab] = useState<TabKey>('overview')
  const [selectedPid, setSelectedPid] = useState<number | null>(null)
  const [history, setHistory] = useState<HistoryPoint[]>([])
  const [events, setEvents] = useState<SchedEvent[]>([])
  const [wakeups, setWakeups] = useState<WakeupEntry[]>([])
  const [detailEvents, setDetailEvents] = useState<SchedEvent[]>([])
  const intervalRef = useRef<number>()

  const handleWsMessage = useCallback((msg: any) => {
    const parsed = parseSchedData(msg)
    if (parsed.length > 0) setSummaries(parsed)
  }, [])

  const { connectionState: wsState } = useWebSocket({
    pipelineKey: 'sched_analysis',
    onMessage: handleWsMessage,
  })

  const fetchSummary = useCallback(async () => {
    try {
      const pipeRes = await fetch('/api/v1/pipelines')
      const pipeData = await pipeRes.json()
      const schedPipeline = (pipeData.pipelines || []).find((p: any) => p.name === 'sched_analysis')
      setStub(schedPipeline?.stub === true)

      const res = await fetch('/api/v1/cpu/sched/summary')
      if (!res.ok) return
      const json = await res.json()
      const parsed = parseSchedData(json)
      setSummaries(parsed)
      setError(null)
    } catch (e: any) {
      setError(e.message)
    }
  }, [])

  const fetchHistory = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/sched/history')
      if (!res.ok) return
      const json = await res.json()
      if (json.history) setHistory(json.history)
    } catch {}
  }, [])

  const fetchEvents = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/sched/events?limit=500')
      if (!res.ok) return
      const json = await res.json()
      if (json.events) setEvents(json.events)
    } catch {}
  }, [])

  const fetchWakeups = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/sched/wakeups')
      if (!res.ok) return
      const json = await res.json()
      if (json.wakeups) setWakeups(json.wakeups)
    } catch {}
  }, [])

  const fetchDetailEvents = useCallback(async (pid: number) => {
    try {
      const res = await fetch(`/api/v1/cpu/sched/events?pid=${pid}&limit=200`)
      if (!res.ok) return
      const json = await res.json()
      if (json.events) setDetailEvents(json.events)
    } catch {}
  }, [])

  useEffect(() => {
    fetchSummary()
    const pollMs = wsState === 'connected' ? 15000 : 5000
    intervalRef.current = window.setInterval(fetchSummary, pollMs)
    return () => clearInterval(intervalRef.current)
  }, [fetchSummary, wsState])

  useEffect(() => {
    if (activeTab === 'timeseries') {
      fetchHistory()
      const id = window.setInterval(fetchHistory, 5000)
      return () => clearInterval(id)
    }
    if (activeTab === 'migrations') {
      fetchEvents()
      const id = window.setInterval(fetchEvents, 5000)
      return () => clearInterval(id)
    }
    if (activeTab === 'wakeups') {
      fetchWakeups()
      const id = window.setInterval(fetchWakeups, 10000)
      return () => clearInterval(id)
    }
  }, [activeTab, fetchHistory, fetchEvents, fetchWakeups])

  useEffect(() => {
    if (selectedPid !== null) fetchDetailEvents(selectedPid)
  }, [selectedPid, fetchDetailEvents])

  const sorted = [...summaries].sort((a, b) => {
    if (sortBy === 'switches') return b.switch_count - a.switch_count
    if (sortBy === 'latency') return b.avg_runqueue_latency_us - a.avg_runqueue_latency_us
    return b.migrate_count - a.migrate_count
  })

  const histogram = buildLatencyHistogram(summaries)
  const totalSwitches = summaries.reduce((s, x) => s + x.switch_count, 0)
  const avgLatency = summaries.length > 0
    ? summaries.reduce((s, x) => s + x.avg_runqueue_latency_us, 0) / summaries.length : 0
  const maxLatency = Math.max(...summaries.map(x => x.max_runqueue_latency_us), 0)

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16, flexWrap: 'wrap', gap: 8 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <h2 style={{ margin: 0, fontSize: 22 }}>Scheduler Analysis</h2>
          <WsIndicator state={wsState} />
        </div>
        <div style={{ display: 'flex', gap: 6 }}>
          {(['overview', 'timeseries', 'migrations', 'wakeups'] as TabKey[]).map(t => (
            <button key={t} style={tabBtnStyle(activeTab === t)} onClick={() => setActiveTab(t)}>
              {t === 'overview' ? 'Overview' : t === 'timeseries' ? 'Time Series' : t === 'migrations' ? 'Migrations' : 'Wakeups'}
            </button>
          ))}
          <button style={btnStyle} onClick={fetchSummary}>Refresh</button>
        </div>
      </div>

      {error && (
        <div style={{ background: '#331a1a', border: '1px solid #7f1d1d', borderRadius: 8, padding: 12, marginBottom: 16, color: '#f87171', fontSize: 13 }}>
          {error}
        </div>
      )}

      {stub && summaries.length === 0 && (
        <div style={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 8, padding: 48, textAlign: 'center', marginBottom: 16 }}>
          <div style={{ fontSize: 36, marginBottom: 12 }}>🔌</div>
          <div style={{ fontSize: 16, color: '#f59e0b', marginBottom: 8 }}>
            Scheduler Analyzer is in stub mode
          </div>
          <div style={{ fontSize: 13, color: '#888', maxWidth: 480, margin: '0 auto' }}>
            The eBPF-based scheduler analyzer requires a compiled BPF object. Configure <code style={{ color: '#93c5fd' }}>bpf_object</code> path
            in the sched_analysis pipeline config, or compile probes with <code style={{ color: '#93c5fd' }}>bazel build //src/ebpf/probes:all</code>.
          </div>
        </div>
      )}

      {activeTab === 'overview' && (
        <OverviewTab
          summaries={sorted} histogram={histogram} sortBy={sortBy} setSortBy={setSortBy}
          totalSwitches={totalSwitches} avgLatency={avgLatency} maxLatency={maxLatency}
          onSelectPid={setSelectedPid} selectedPid={selectedPid} detailEvents={detailEvents}
        />
      )}
      {activeTab === 'timeseries' && <TimeSeriesTab history={history} />}
      {activeTab === 'migrations' && <MigrationsTab events={events} />}
      {activeTab === 'wakeups' && <WakeupsTab wakeups={wakeups} />}
    </div>
  )
}

function OverviewTab({
  summaries, histogram, sortBy, setSortBy,
  totalSwitches, avgLatency, maxLatency,
  onSelectPid, selectedPid, detailEvents,
}: {
  summaries: SchedSummary[]
  histogram: LatencyBucket[]
  sortBy: string
  setSortBy: (s: 'switches' | 'latency' | 'migrations') => void
  totalSwitches: number
  avgLatency: number
  maxLatency: number
  onSelectPid: (pid: number | null) => void
  selectedPid: number | null
  detailEvents: SchedEvent[]
}) {
  return (
    <>
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 16, marginBottom: 24 }}>
        <div style={card}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Total Context Switches</div>
          <div style={{ fontSize: 28, fontWeight: 700, color: '#60a5fa' }}>{totalSwitches.toLocaleString()}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Avg Runqueue Latency</div>
          <div style={{ fontSize: 28, fontWeight: 700, color: '#60a5fa' }}>{avgLatency.toFixed(1)} us</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Max Runqueue Latency</div>
          <div style={{ fontSize: 28, fontWeight: 700, color: maxLatency > 5000 ? '#f87171' : '#60a5fa' }}>
            {maxLatency > 1000 ? `${(maxLatency / 1000).toFixed(1)} ms` : `${maxLatency} us`}
          </div>
        </div>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 16, marginBottom: 24 }}>
        <div style={card}>
          <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Runqueue Latency Distribution</h3>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={histogram}>
              <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
              <XAxis dataKey="range" tick={{ fill: '#888', fontSize: 11 }} />
              <YAxis tick={{ fill: '#888', fontSize: 11 }} />
              <Tooltip contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }} />
              <Bar dataKey="count" fill="#3b82f6" radius={[4, 4, 0, 0]} />
            </BarChart>
          </ResponsiveContainer>
        </div>
        <div style={card}>
          <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Top Processes by Switches</h3>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={summaries.slice(0, 8)} layout="vertical">
              <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
              <XAxis type="number" tick={{ fill: '#888', fontSize: 11 }} />
              <YAxis dataKey="comm" type="category" tick={{ fill: '#888', fontSize: 11 }} width={100} />
              <Tooltip contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }} />
              <Bar dataKey="switch_count" fill="#10b981" radius={[0, 4, 4, 0]} />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </div>

      <div style={card}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
          <h3 style={{ margin: 0, fontSize: 16 }}>Per-Process Scheduler Statistics</h3>
          <div style={{ display: 'flex', gap: 8 }}>
            {(['switches', 'latency', 'migrations'] as const).map(key => (
              <button key={key} onClick={() => setSortBy(key)}
                style={tabBtnStyle(sortBy === key)}
              >
                {key === 'switches' ? 'Switches' : key === 'latency' ? 'Latency' : 'Migrations'}
              </button>
            ))}
          </div>
        </div>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid #2a2d35' }}>
              <th style={thStyle}>PID</th>
              <th style={thStyle}>Command</th>
              <th style={thStyle}>Ctx Switches</th>
              <th style={thStyle}>Avg Latency</th>
              <th style={thStyle}>Max Latency</th>
              <th style={thStyle}>CPU Migrations</th>
            </tr>
          </thead>
          <tbody>
            {summaries.map(s => (
              <React.Fragment key={s.pid}>
                <tr
                  style={{
                    borderBottom: '1px solid #1f2228',
                    cursor: 'pointer',
                    background: selectedPid === s.pid ? '#252830' : 'transparent',
                  }}
                  onClick={() => onSelectPid(selectedPid === s.pid ? null : s.pid)}
                >
                  <td style={tdStyle}>{s.pid}</td>
                  <td style={{ ...tdStyle, fontFamily: 'monospace' }}>{s.comm}</td>
                  <td style={tdStyle}>{s.switch_count.toLocaleString()}</td>
                  <td style={tdStyle}>
                    <span style={{ color: s.avg_runqueue_latency_us > 500 ? '#f87171' : s.avg_runqueue_latency_us > 100 ? '#f59e0b' : '#4ade80' }}>
                      {s.avg_runqueue_latency_us.toFixed(1)} us
                    </span>
                  </td>
                  <td style={tdStyle}>
                    <span style={{ color: s.max_runqueue_latency_us > 5000 ? '#f87171' : '#888' }}>
                      {s.max_runqueue_latency_us > 1000 ? `${(s.max_runqueue_latency_us / 1000).toFixed(1)} ms` : `${s.max_runqueue_latency_us} us`}
                    </span>
                  </td>
                  <td style={tdStyle}>{s.migrate_count}</td>
                </tr>
                {selectedPid === s.pid && (
                  <tr>
                    <td colSpan={6} style={{ padding: 0 }}>
                      <ProcessDetail pid={s.pid} comm={s.comm} events={detailEvents} />
                    </td>
                  </tr>
                )}
              </React.Fragment>
            ))}
          </tbody>
        </table>
      </div>
    </>
  )
}

function ProcessDetail({ pid, comm, events }: { pid: number; comm: string; events: SchedEvent[] }) {
  if (events.length === 0) {
    return (
      <div style={{ padding: 16, color: '#888', fontSize: 13, background: '#15171c' }}>
        No detailed events available for {comm} (pid {pid}). Enable detailed_mode in sched_analysis pipeline.
      </div>
    )
  }
  return (
    <div style={{ background: '#15171c', padding: 16 }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 14, color: '#e0e0e0' }}>
        Recent events for {comm} (pid {pid}) — {events.length} events
      </h4>
      <div style={{ maxHeight: 300, overflowY: 'auto' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
          <thead>
            <tr style={{ borderBottom: '1px solid #2a2d35' }}>
              <th style={thSmall}>Time</th>
              <th style={thSmall}>Type</th>
              <th style={thSmall}>From</th>
              <th style={thSmall}>To</th>
              <th style={thSmall}>CPU</th>
              <th style={thSmall}>Latency</th>
            </tr>
          </thead>
          <tbody>
            {events.map((e, i) => (
              <tr key={i} style={{ borderBottom: '1px solid #1a1d23' }}>
                <td style={tdSmall}>{new Date(e.timestamp_ms).toLocaleTimeString()}</td>
                <td style={tdSmall}>
                  <span style={{
                    padding: '2px 6px', borderRadius: 4, fontSize: 10,
                    background: e.event_type === 'switch' ? '#1e3a5f' :
                                e.event_type === 'wakeup' ? '#1e3f1e' : '#3f3a1e',
                    color: e.event_type === 'switch' ? '#60a5fa' :
                           e.event_type === 'wakeup' ? '#4ade80' : '#fbbf24',
                  }}>
                    {e.event_type}
                  </span>
                </td>
                <td style={tdSmall}>{e.prev_comm} ({e.prev_pid})</td>
                <td style={tdSmall}>{e.next_comm} ({e.next_pid})</td>
                <td style={tdSmall}>{e.cpu}</td>
                <td style={tdSmall}>
                  {e.latency_ns > 1000000
                    ? `${(e.latency_ns / 1000000).toFixed(1)} ms`
                    : `${(e.latency_ns / 1000).toFixed(0)} us`}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}

function TimeSeriesTab({ history }: { history: HistoryPoint[] }) {
  const chartData = useMemo(() =>
    history.map(p => ({
      time: p.timestamp_ms,
      switches: p.total_switches,
      avg_lat_us: Math.round(p.avg_latency_us * 10) / 10,
      max_lat_us: Math.round(p.max_latency_ns / 1000),
      migrations: p.total_migrations,
    }))
  , [history])

  if (chartData.length === 0) {
    return (
      <div style={card}>
        <p style={{ color: '#888', fontSize: 14, textAlign: 'center', padding: 40 }}>
          Collecting history data... Data will appear after a few collection cycles.
        </p>
      </div>
    )
  }

  return (
    <div style={{ display: 'grid', gap: 20 }}>
      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Context Switches Over Time</h3>
        <ResponsiveContainer width="100%" height={250}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
            <XAxis dataKey="time" type="number" domain={['dataMin', 'dataMax']}
              tickFormatter={ts => new Date(ts).toLocaleTimeString()}
              stroke="#888" tick={{ fill: '#888', fontSize: 11 }} />
            <YAxis stroke="#888" tick={{ fill: '#888', fontSize: 11 }} />
            <Tooltip contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }}
              labelFormatter={ts => new Date(ts as number).toLocaleTimeString()} />
            <Legend wrapperStyle={{ fontSize: 12 }} />
            <Line type="monotone" dataKey="switches" name="Context Switches" stroke="#3b82f6" dot={false} strokeWidth={2} />
            <Line type="monotone" dataKey="migrations" name="Migrations" stroke="#f59e0b" dot={false} strokeWidth={2} />
          </LineChart>
        </ResponsiveContainer>
      </div>
      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Runqueue Latency Over Time</h3>
        <ResponsiveContainer width="100%" height={250}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
            <XAxis dataKey="time" type="number" domain={['dataMin', 'dataMax']}
              tickFormatter={ts => new Date(ts).toLocaleTimeString()}
              stroke="#888" tick={{ fill: '#888', fontSize: 11 }} />
            <YAxis stroke="#888" tick={{ fill: '#888', fontSize: 11 }} unit=" us" />
            <Tooltip contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }}
              labelFormatter={ts => new Date(ts as number).toLocaleTimeString()} />
            <Legend wrapperStyle={{ fontSize: 12 }} />
            <Line type="monotone" dataKey="avg_lat_us" name="Avg Latency (us)" stroke="#4ade80" dot={false} strokeWidth={2} />
            <Line type="monotone" dataKey="max_lat_us" name="Max Latency (us)" stroke="#f87171" dot={false} strokeWidth={1.5} strokeDasharray="5 3" />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  )
}

function MigrationsTab({ events }: { events: SchedEvent[] }) {
  const migrateEvents = useMemo(() =>
    events.filter(e => e.event_type === 'migrate')
  , [events])

  const cpuSet = useMemo(() => {
    const s = new Set<number>()
    for (const e of events) s.add(e.cpu)
    return [...s].sort((a, b) => a - b)
  }, [events])

  const commColors = useMemo(() => {
    const colors = ['#3b82f6', '#ef4444', '#10b981', '#f59e0b', '#a855f7', '#ec4899', '#06b6d4', '#84cc16']
    const map = new Map<string, string>()
    const comms = new Set<string>()
    for (const e of migrateEvents) {
      comms.add(e.next_comm || e.prev_comm)
    }
    let i = 0
    for (const c of comms) {
      map.set(c, colors[i % colors.length]!)
      i++
    }
    return map
  }, [migrateEvents])

  if (migrateEvents.length === 0) {
    return (
      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>CPU Migration Events</h3>
        <p style={{ color: '#888', fontSize: 14, textAlign: 'center', padding: 40 }}>
          No migration events captured yet. Enable detailed_mode and track_migrations in sched_analysis.
        </p>
      </div>
    )
  }

  const timeMin = migrateEvents.length > 0 ? migrateEvents[0]!.timestamp_ms : 0
  const timeMax = migrateEvents.length > 0 ? migrateEvents[migrateEvents.length - 1]!.timestamp_ms : 1
  const timeRange = Math.max(timeMax - timeMin, 1)
  const svgWidth = 800
  const svgHeight = Math.max(120, cpuSet.length * 40 + 40)
  const laneH = 30

  return (
    <div style={card}>
      <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>CPU Migration Swimlane ({migrateEvents.length} events)</h3>
      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap', marginBottom: 12 }}>
        {[...commColors.entries()].map(([comm, color]) => (
          <span key={comm} style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 11, color: '#b0b0b0' }}>
            <span style={{ width: 10, height: 10, borderRadius: 2, background: color, display: 'inline-block' }} />
            {comm}
          </span>
        ))}
      </div>
      <div style={{ overflowX: 'auto' }}>
        <svg width={svgWidth} height={svgHeight} style={{ display: 'block' }}>
          {cpuSet.map((cpu, i) => {
            const y = 20 + i * 40
            return (
              <g key={cpu}>
                <text x={0} y={y + laneH / 2 + 4} fill="#888" fontSize={11} fontFamily="monospace">cpu{cpu}</text>
                <rect x={50} y={y} width={svgWidth - 60} height={laneH} rx={4} fill="#1f2228" stroke="#2a2d35" strokeWidth={0.5} />
              </g>
            )
          })}
          {migrateEvents.map((e, i) => {
            const cpuIdx = cpuSet.indexOf(e.cpu)
            if (cpuIdx < 0) return null
            const x = 50 + ((e.timestamp_ms - timeMin) / timeRange) * (svgWidth - 60)
            const y = 20 + cpuIdx * 40 + laneH / 2
            const comm = e.next_comm || e.prev_comm
            const color = commColors.get(comm) || '#888'
            return (
              <g key={i}>
                <circle cx={x} cy={y} r={4} fill={color} opacity={0.8}>
                  <title>{comm} (pid {e.next_pid}) → cpu{e.cpu} @{new Date(e.timestamp_ms).toLocaleTimeString()}</title>
                </circle>
              </g>
            )
          })}
        </svg>
      </div>
    </div>
  )
}

function WakeupsTab({ wakeups }: { wakeups: WakeupEntry[] }) {
  const pairs = useMemo(() => {
    const map = new Map<string, { waker: string; wakee: string; count: number }>()
    for (const w of wakeups) {
      const key = `${w.waker_comm}->${w.wakee_comm}`
      const existing = map.get(key)
      if (existing) existing.count++
      else map.set(key, { waker: w.waker_comm, wakee: w.wakee_comm, count: 1 })
    }
    return [...map.values()].sort((a, b) => b.count - a.count)
  }, [wakeups])

  const nodes = useMemo(() => {
    const s = new Set<string>()
    for (const p of pairs) { s.add(p.waker); s.add(p.wakee) }
    return [...s]
  }, [pairs])

  if (wakeups.length === 0) {
    return (
      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Wakeup Chain Analysis</h3>
        <p style={{ color: '#888', fontSize: 14, textAlign: 'center', padding: 40 }}>
          No wakeup data available yet. Enable detailed_mode in sched_analysis.
        </p>
      </div>
    )
  }

  const maxCount = pairs.length > 0 ? pairs[0]!.count : 1

  const svgW = 700
  const svgH = Math.max(200, nodes.length * 32 + 40)
  const nodePositions = new Map<string, { x: number; y: number }>()
  const leftNodes = nodes.filter((_, i) => i % 2 === 0)
  const rightNodes = nodes.filter((_, i) => i % 2 === 1)
  leftNodes.forEach((n, i) => nodePositions.set(n, { x: 120, y: 30 + i * 50 }))
  rightNodes.forEach((n, i) => nodePositions.set(n, { x: svgW - 120, y: 30 + i * 50 }))

  return (
    <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 20 }}>
      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Wakeup Chain Graph</h3>
        <div style={{ overflowX: 'auto' }}>
          <svg width={svgW} height={svgH}>
            <defs>
              <marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5"
                markerWidth="6" markerHeight="6" orient="auto-start-reverse">
                <path d="M 0 0 L 10 5 L 0 10 z" fill="#888" />
              </marker>
            </defs>
            {pairs.slice(0, 30).map((p, i) => {
              const from = nodePositions.get(p.waker)
              const to = nodePositions.get(p.wakee)
              if (!from || !to) return null
              const strokeW = Math.max(1, (p.count / maxCount) * 4)
              return (
                <line key={i} x1={from.x + 50} y1={from.y} x2={to.x - 50} y2={to.y}
                  stroke="#3b82f6" strokeWidth={strokeW} opacity={0.5}
                  markerEnd="url(#arrow)">
                  <title>{p.waker} → {p.wakee}: {p.count} wakeups</title>
                </line>
              )
            })}
            {[...nodePositions.entries()].map(([name, pos]) => (
              <g key={name}>
                <rect x={pos.x - 48} y={pos.y - 12} width={96} height={24} rx={12}
                  fill="#252830" stroke="#3b82f6" strokeWidth={0.5} />
                <text x={pos.x} y={pos.y + 4} textAnchor="middle"
                  fill="#e0e0e0" fontSize={10} fontFamily="monospace">{name}</text>
              </g>
            ))}
          </svg>
        </div>
      </div>

      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Top Waker-Wakee Pairs</h3>
        <div style={{ maxHeight: 500, overflowY: 'auto' }}>
          <table style={{ width: '100%', borderCollapse: 'collapse' }}>
            <thead>
              <tr style={{ borderBottom: '1px solid #2a2d35' }}>
                <th style={thSmall}>Waker</th>
                <th style={thSmall}>Wakee</th>
                <th style={thSmall}>Count</th>
                <th style={thSmall}>Frequency</th>
              </tr>
            </thead>
            <tbody>
              {pairs.slice(0, 50).map((p, i) => (
                <tr key={i} style={{ borderBottom: '1px solid #1f2228' }}>
                  <td style={{ ...tdSmall, fontFamily: 'monospace' }}>{p.waker}</td>
                  <td style={{ ...tdSmall, fontFamily: 'monospace' }}>{p.wakee}</td>
                  <td style={tdSmall}>{p.count}</td>
                  <td style={tdSmall}>
                    <div style={{ width: 80, height: 6, background: '#1f2228', borderRadius: 3 }}>
                      <div style={{ width: `${(p.count / maxCount) * 100}%`, height: '100%', background: '#3b82f6', borderRadius: 3 }} />
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  )
}

const thStyle: React.CSSProperties = {
  textAlign: 'left', padding: '10px 12px', fontSize: 12,
  color: '#888', fontWeight: 600, textTransform: 'uppercase',
}

const tdStyle: React.CSSProperties = {
  padding: '10px 12px', fontSize: 13,
}

const thSmall: React.CSSProperties = {
  textAlign: 'left', padding: '6px 8px', fontSize: 11,
  color: '#888', fontWeight: 600,
}

const tdSmall: React.CSSProperties = {
  padding: '6px 8px', fontSize: 12,
}
