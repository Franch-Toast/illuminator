import React, { useState, useEffect, useRef, useCallback } from 'react'
import {
  ResponsiveContainer,
  BarChart,
  Bar,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
} from 'recharts'

interface SchedSummary {
  pid: number
  comm: string
  switch_count: number
  avg_runqueue_latency_us: number
  max_runqueue_latency_us: number
  migrate_count: number
}

interface LatencyBucket {
  range: string
  count: number
}

const card: React.CSSProperties = {
  background: '#1a1d23', borderRadius: 8, padding: 20,
  border: '1px solid #2a2d35',
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '6px 14px', fontSize: 13, cursor: 'pointer',
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

const DEMO_SUMMARIES: SchedSummary[] = [
  { pid: 1234, comm: 'planning', switch_count: 12345, avg_runqueue_latency_us: 45, max_runqueue_latency_us: 1200, migrate_count: 234 },
  { pid: 5678, comm: 'perception', switch_count: 8901, avg_runqueue_latency_us: 120, max_runqueue_latency_us: 3400, migrate_count: 156 },
  { pid: 9012, comm: 'map_engine', switch_count: 5600, avg_runqueue_latency_us: 23, max_runqueue_latency_us: 890, migrate_count: 89 },
  { pid: 3456, comm: 'localization', switch_count: 4200, avg_runqueue_latency_us: 67, max_runqueue_latency_us: 2100, migrate_count: 112 },
  { pid: 7890, comm: 'io_worker', switch_count: 3100, avg_runqueue_latency_us: 350, max_runqueue_latency_us: 8900, migrate_count: 45 },
  { pid: 2345, comm: 'data_logger', switch_count: 1200, avg_runqueue_latency_us: 15, max_runqueue_latency_us: 450, migrate_count: 23 },
]

export default function Timeline() {
  const [summaries, setSummaries] = useState<SchedSummary[]>(DEMO_SUMMARIES)
  const [error, setError] = useState<string | null>(null)
  const [sortBy, setSortBy] = useState<'switches' | 'latency' | 'migrations'>('switches')
  const intervalRef = useRef<number>()

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch('/api/v1/cpu/sched/summary')
      if (!res.ok) return
      const json = await res.json()
      const parsed = parseSchedData(json)
      if (parsed.length > 0) setSummaries(parsed)
      setError(null)
    } catch (e: any) {
      setError(e.message)
    }
  }, [])

  useEffect(() => {
    fetchData()
    intervalRef.current = window.setInterval(fetchData, 5000)
    return () => clearInterval(intervalRef.current)
  }, [fetchData])

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
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>Scheduler Analysis</h2>
        <div style={{ display: 'flex', gap: 8 }}>
          <button style={btnStyle} onClick={fetchData}>Refresh</button>
        </div>
      </div>

      {error && (
        <div style={{ background: '#331a1a', border: '1px solid #7f1d1d', borderRadius: 8, padding: 12, marginBottom: 16, color: '#f87171', fontSize: 13 }}>
          {error}
        </div>
      )}

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
              <Tooltip
                contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }}
                labelStyle={{ color: '#e0e0e0' }}
              />
              <Bar dataKey="count" fill="#3b82f6" radius={[4, 4, 0, 0]} />
            </BarChart>
          </ResponsiveContainer>
        </div>

        <div style={card}>
          <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Top Processes by Switches</h3>
          <ResponsiveContainer width="100%" height={200}>
            <BarChart data={sorted.slice(0, 8)} layout="vertical">
              <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
              <XAxis type="number" tick={{ fill: '#888', fontSize: 11 }} />
              <YAxis dataKey="comm" type="category" tick={{ fill: '#888', fontSize: 11 }} width={100} />
              <Tooltip
                contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6 }}
                labelStyle={{ color: '#e0e0e0' }}
              />
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
                style={{
                  ...btnStyle,
                  background: sortBy === key ? '#1d4ed8' : '#374151',
                  fontSize: 12, padding: '4px 10px',
                }}
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
            {sorted.map(s => (
              <tr key={s.pid} style={{ borderBottom: '1px solid #1f2228' }}>
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
            ))}
          </tbody>
        </table>
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
