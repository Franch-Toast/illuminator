import React, { useEffect, useState, useCallback, useRef } from 'react'
import { ResponsiveContainer, LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, Legend } from 'recharts'
import { api } from '../services/apiClient'
import { usePipelineStore } from '../stores/usePipelineStore'
import { useTimeStore } from '../stores/useTimeStore'
import { timeSeriesStore, type DataPoint } from '../services/timeSeriesStore'
import { colors } from '../styles/theme'

const card: React.CSSProperties = {
  background: colors.cardBg, borderRadius: 8, padding: 16,
  border: `1px solid ${colors.cardBorder}`,
}

interface HealthInfo {
  status: string
  version: string
}

export default function SystemPage() {
  const { pipelines } = usePipelineStore()
  const { mode, range } = useTimeStore()
  const [health, setHealth] = useState<HealthInfo | null>(null)
  const [metrics, setMetrics] = useState<any>(null)
  const [channelHistory, setChannelHistory] = useState<DataPoint[]>([])
  const intervalRef = useRef<ReturnType<typeof setInterval>>()

  const fetchHealth = useCallback(async () => {
    try {
      const data = await api.healthz()
      setHealth(data)
    } catch {
      setHealth(null)
    }
  }, [])

  const fetchMetrics = useCallback(async () => {
    try {
      const data = await api.internalMetrics()
      setMetrics(data)

      const pipeData = await api.channelStats()
      for (const ch of (pipeData.channels || [])) {
        timeSeriesStore.append(`channel.${ch.pipeline}`, Date.now(), {
          size: ch.size, utilization: ch.utilization * 100,
          enqueued: ch.enqueued, dropped: ch.dropped,
        })
      }
    } catch { /* ignore */ }
  }, [])

  useEffect(() => {
    fetchHealth()
  }, [fetchHealth])

  useEffect(() => {
    if (mode === 'paused') return
    fetchMetrics()
    intervalRef.current = setInterval(fetchMetrics, 3000)
    return () => clearInterval(intervalRef.current)
  }, [fetchMetrics, mode])

  useEffect(() => {
    const keys = pipelines.map(p => `channel.${p.name}`)
    const update = () => {
      const merged: DataPoint[] = []
      for (const key of keys) {
        const data = timeSeriesStore.query(key, range.start, range.end)
        for (const d of data) {
          merged.push({ ...d, _pipeline: key.replace('channel.', '') } as any)
        }
      }
      setChannelHistory(merged)
    }
    update()
    const unsubs = keys.map(k => timeSeriesStore.subscribe(k, update))
    return () => unsubs.forEach(fn => fn())
  }, [pipelines, range.start, range.end])

  const totalBatches = pipelines.reduce((s, p) => s + p.batches, 0)
  const totalRecords = pipelines.reduce((s, p) => s + p.records, 0)
  const totalErrors = pipelines.reduce((s, p) => s + p.errors, 0)
  const totalDrops = pipelines.reduce((s, p) => s + (p.channel?.dropped || 0), 0)

  return (
    <div style={{ padding: 20 }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 20 }}>System</h2>

      {/* Health Status */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))', gap: 12, marginBottom: 16 }}>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Status</div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{
              width: 10, height: 10, borderRadius: '50%',
              background: health?.status === 'ok' ? '#4ade80' : '#f87171',
            }} />
            <span style={{ fontSize: 18, fontWeight: 700, color: health?.status === 'ok' ? '#4ade80' : '#f87171' }}>
              {health?.status?.toUpperCase() ?? 'OFFLINE'}
            </span>
          </div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Version</div>
          <div style={{ fontSize: 18, fontWeight: 700, color: colors.accent }}>{health?.version ?? '—'}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Pipelines</div>
          <div style={{ fontSize: 18, fontWeight: 700, color: colors.accent }}>
            {pipelines.filter(p => p.running).length} / {pipelines.length}
          </div>
        </div>
      </div>

      {/* Aggregate Stats */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 12, marginBottom: 16 }}>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Total Batches</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: colors.textPrimary }}>{totalBatches.toLocaleString()}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Total Records</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: colors.textPrimary }}>{totalRecords.toLocaleString()}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Total Errors</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: totalErrors > 0 ? '#f87171' : colors.textPrimary }}>
            {totalErrors.toLocaleString()}
          </div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', marginBottom: 6 }}>Total Drops</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: totalDrops > 0 ? '#f87171' : colors.textPrimary }}>
            {totalDrops.toLocaleString()}
          </div>
        </div>
      </div>

      {/* Per-Pipeline Detail */}
      <div style={card}>
        <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>Pipeline Details</h3>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
          <thead>
            <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              {['Name', 'Status', 'Batches', 'Records', 'Errors', 'Channel Size', 'Enqueued', 'Dequeued', 'Dropped', 'BP Events'].map(h => (
                <th key={h} style={{
                  padding: '6px 8px', textAlign: 'left', color: '#888',
                  fontWeight: 500, fontSize: 10, textTransform: 'uppercase',
                }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {pipelines.map(p => (
              <tr key={p.name} style={{ borderBottom: `1px solid #1f2228` }}>
                <td style={{ padding: '6px 8px', fontWeight: 600 }}>{p.name}</td>
                <td style={{ padding: '6px 8px' }}>
                  <span style={{
                    padding: '1px 6px', borderRadius: 4, fontSize: 10,
                    background: p.running ? 'rgba(74,222,128,0.1)' : 'rgba(107,114,128,0.1)',
                    color: p.running ? '#4ade80' : '#888',
                  }}>
                    {p.running ? 'RUN' : p.stub ? 'STUB' : 'STOP'}
                  </span>
                </td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>{p.batches.toLocaleString()}</td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>{p.records.toLocaleString()}</td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace', color: p.errors > 0 ? '#f87171' : '#888' }}>
                  {p.errors}
                </td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>
                  {p.channel ? `${p.channel.size}/${p.channel.capacity}` : '—'}
                </td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>{p.channel?.enqueued.toLocaleString() ?? '—'}</td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>{p.channel?.dequeued.toLocaleString() ?? '—'}</td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace',
                  color: p.channel && p.channel.dropped > 0 ? '#f87171' : '#888' }}>
                  {p.channel?.dropped ?? '—'}
                </td>
                <td style={{ padding: '6px 8px', fontFamily: 'monospace' }}>{p.channel?.backpressure_events ?? '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* Internal Metrics */}
      {metrics && typeof metrics === 'object' && (
        <div style={{ ...card, marginTop: 16 }}>
          <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>Internal Metrics (Raw)</h3>
          <pre style={{
            background: colors.bg, padding: 12, borderRadius: 6,
            fontSize: 11, overflow: 'auto', maxHeight: 300,
            border: `1px solid ${colors.cardBorder}`,
          }}>
            {JSON.stringify(metrics, null, 2)}
          </pre>
        </div>
      )}
    </div>
  )
}
