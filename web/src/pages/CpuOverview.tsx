import React, { useMemo, useState } from 'react'
import {
  ResponsiveContainer,
  AreaChart,
  Area,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
} from 'recharts'
import { useCpuUtilization } from '../hooks/useCpuMetrics'
import { usePipelines } from '../hooks/useApi'
import { card as themeCard, colors } from '../styles/theme'

const card: React.CSSProperties = {
  background: '#1a1d23',
  borderRadius: 8,
  padding: 20,
  border: '1px solid #2a2d35',
}

const COLORS = {
  user: '#3b82f6',
  system: '#ef4444',
  iowait: '#f59e0b',
  irq: '#a855f7',
  steal: '#6b7280',
}

function busyHeatColor(busyPct: number): string {
  const x = Math.max(0, Math.min(100, busyPct)) / 100
  let r: number
  let g: number
  let b: number
  if (x < 0.5) {
    const t = x * 2
    r = Math.round(34 + (234 - 34) * t)
    g = Math.round(197 + (179 - 197) * t)
    b = Math.round(94 + (8 - 94) * t)
  } else {
    const t = (x - 0.5) * 2
    r = Math.round(234 + (239 - 234) * t)
    g = Math.round(179 + (68 - 179) * t)
    b = Math.round(8 + (68 - 8) * t)
  }
  return `rgb(${r},${g},${b})`
}

function coreSortKey(cpu: string): number {
  const m = cpu.match(/\d+/g)
  if (!m?.length) return 0
  return parseInt(m[m.length - 1]!, 10)
}

export default function CpuOverview() {
  const { data, history, error } = useCpuUtilization(1000)
  const [rangeMinutes, setRangeMinutes] = useState<1 | 5 | 15>(1)

  const chartData = useMemo(() => {
    if (history.length === 0) return []
    const latest = history[history.length - 1]!.time
    const cutoff = latest - rangeMinutes * 60 * 1000
    return history.filter((h) => h.time >= cutoff)
  }, [history, rangeMinutes])

  const sortedCores = useMemo(() => {
    return [...data.cores].sort((a, b) => coreSortKey(a.cpu) - coreSortKey(b.cpu))
  }, [data.cores])

  const heatmapWidth = 280

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 24, flexWrap: 'wrap', gap: 12 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <h2 style={{ margin: 0, fontSize: 22 }}>CPU Overview</h2>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span style={{ fontSize: 12, color: '#888' }}>Chart range</span>
          {([1, 5, 15] as const).map((m) => (
            <button
              key={m}
              type="button"
              onClick={() => setRangeMinutes(m)}
              style={{
                padding: '6px 14px',
                borderRadius: 6,
                border: `1px solid ${rangeMinutes === m ? '#60a5fa' : '#2a2d35'}`,
                background: rangeMinutes === m ? '#252830' : '#1a1d23',
                color: rangeMinutes === m ? '#60a5fa' : '#b0b0b0',
                cursor: 'pointer',
                fontSize: 13,
              }}
            >
              {m}m
            </button>
          ))}
        </div>
      </div>

      {error && (
        <div style={{ ...card, marginBottom: 16, borderColor: '#7f1d1d', color: '#f87171' }}>
          {error}
        </div>
      )}

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 340px', gap: 20, marginBottom: 20, alignItems: 'start' }}>
        <div style={card}>
          <h3 style={{ margin: '0 0 16px', fontSize: 15, fontWeight: 600, color: '#e0e0e0' }}>
            System CPU utilization
          </h3>
          <div style={{ width: '100%', height: 320 }}>
            <ResponsiveContainer width="100%" height="100%">
              <AreaChart data={chartData} margin={{ top: 8, right: 12, left: 0, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
                <XAxis
                  dataKey="time"
                  type="number"
                  domain={['dataMin', 'dataMax']}
                  tickFormatter={(ts) => new Date(ts as number).toLocaleTimeString()}
                  stroke="#888"
                  tick={{ fill: '#888', fontSize: 11 }}
                />
                <YAxis
                  domain={[0, 100]}
                  stroke="#888"
                  tick={{ fill: '#888', fontSize: 11 }}
                  tickFormatter={(v) => `${v}%`}
                />
                <Tooltip
                  contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 8 }}
                  labelFormatter={(ts) => new Date(ts as number).toLocaleString()}
                  formatter={(value: number) => [`${value.toFixed(1)}%`, '']}
                />
                <Legend wrapperStyle={{ fontSize: 12 }} />
                <Area type="monotone" dataKey="user" name="User" stackId="1" stroke={COLORS.user} fill={COLORS.user} fillOpacity={0.85} />
                <Area type="monotone" dataKey="system" name="System" stackId="1" stroke={COLORS.system} fill={COLORS.system} fillOpacity={0.85} />
                <Area type="monotone" dataKey="iowait" name="IO wait" stackId="1" stroke={COLORS.iowait} fill={COLORS.iowait} fillOpacity={0.85} />
                <Area type="monotone" dataKey="irq" name="IRQ" stackId="1" stroke={COLORS.irq} fill={COLORS.irq} fillOpacity={0.85} />
                <Area type="monotone" dataKey="steal" name="Steal" stackId="1" stroke={COLORS.steal} fill={COLORS.steal} fillOpacity={0.85} />
              </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>

        <div style={card}>
          <h3 style={{ margin: '0 0 16px', fontSize: 15, fontWeight: 600 }}>Per-core busy %</h3>
          {sortedCores.length === 0 ? (
            <p style={{ margin: 0, color: '#666', fontSize: 13 }}>No core metrics yet.</p>
          ) : (
            <svg width="100%" height={Math.max(120, sortedCores.length * 28 + 24)} style={{ display: 'block' }}>
              {sortedCores.map((core, i) => {
                const y = 12 + i * 28
                const w = (heatmapWidth * Math.min(100, core.busy_pct)) / 100
                const fill = busyHeatColor(core.busy_pct)
                return (
                  <g key={`${core.cpu}-${i}`}>
                    <text x={0} y={y + 14} fill="#b0b0b0" fontSize={11} style={{ fontFamily: 'monospace' }}>
                      {core.cpu || `cpu${i}`}
                    </text>
                    <rect x={72} y={y} width={heatmapWidth} height={18} rx={4} fill="#252830" stroke="#2a2d35" />
                    <rect x={72} y={y} width={Math.max(0, w)} height={18} rx={4} fill={fill} opacity={0.95} />
                    <text x={72 + heatmapWidth + 8} y={y + 14} fill="#e0e0e0" fontSize={11}>
                      {core.busy_pct.toFixed(1)}%
                    </text>
                  </g>
                )
              })}
            </svg>
          )}
        </div>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(180px, 1fr))', gap: 16 }}>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Load 1m</div>
          <div style={{ fontSize: 26, fontWeight: 700, color: '#60a5fa' }}>{data.loadavg ? data.loadavg.load_1m.toFixed(2) : '—'}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Load 5m</div>
          <div style={{ fontSize: 26, fontWeight: 700, color: '#60a5fa' }}>{data.loadavg ? data.loadavg.load_5m.toFixed(2) : '—'}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Load 15m</div>
          <div style={{ fontSize: 26, fontWeight: 700, color: '#60a5fa' }}>{data.loadavg ? data.loadavg.load_15m.toFixed(2) : '—'}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Ctx switches / s</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: '#e0e0e0' }}>
            {data.counters ? Math.round(data.counters.context_switches_per_sec).toLocaleString() : '—'}
          </div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Interrupts / s</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: '#e0e0e0' }}>
            {data.counters ? Math.round(data.counters.interrupts_per_sec).toLocaleString() : '—'}
          </div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 8 }}>Running / blocked</div>
          <div style={{ fontSize: 22, fontWeight: 700, color: '#e0e0e0' }}>
            {data.runqueue ? `${data.runqueue.procs_running} / ${data.runqueue.procs_blocked}` : '—'}
          </div>
        </div>
      </div>

      {/* Pipeline Channel 状态面板 */}
      <ChannelStatsPanel />
    </div>
  )
}

function ChannelStatsPanel() {
  const { pipelines } = usePipelines(3000)
  const withChannel = pipelines.filter(p => p.channel)

  if (withChannel.length === 0) return null

  return (
    <div style={{ marginTop: 20 }}>
      <h3 style={{ margin: '0 0 12px', fontSize: 15, fontWeight: 600, color: colors.textPrimary }}>
        Pipeline Channels
      </h3>
      <div style={{ overflowX: 'auto' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
          <thead>
            <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              {['Pipeline', 'Capacity', 'Queue', 'Utilization', 'Enqueued', 'Dequeued',
                'Dropped', 'Backpressure'].map(h => (
                <th key={h} style={{
                  padding: '8px 12px', textAlign: 'left',
                  color: colors.textMuted, fontWeight: 500, fontSize: 11,
                  textTransform: 'uppercase', letterSpacing: '0.05em',
                }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {withChannel.map(p => {
              const ch = p.channel!
              const utilPct = ch.capacity > 0
                ? (ch.size / ch.capacity) * 100 : 0
              const barColor = utilPct > 80 ? colors.danger
                : utilPct > 50 ? colors.amber : colors.success
              return (
                <tr key={p.name} style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
                  <td style={{ padding: '8px 12px', fontWeight: 600 }}>{p.name}</td>
                  <td style={{ padding: '8px 12px', color: colors.textSecondary }}>
                    {ch.capacity.toLocaleString()}
                  </td>
                  <td style={{ padding: '8px 12px', color: colors.textSecondary }}>
                    {ch.size.toLocaleString()}
                  </td>
                  <td style={{ padding: '8px 12px' }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                      <div style={{
                        width: 80, height: 6, background: colors.cardBorder,
                        borderRadius: 3, overflow: 'hidden',
                      }}>
                        <div style={{
                          width: `${Math.min(100, utilPct)}%`, height: '100%',
                          background: barColor, borderRadius: 3,
                          transition: 'width 0.3s',
                        }} />
                      </div>
                      <span style={{ fontSize: 11, color: colors.textMuted }}>
                        {utilPct.toFixed(1)}%
                      </span>
                    </div>
                  </td>
                  <td style={{ padding: '8px 12px', fontFamily: 'monospace', color: colors.textSecondary }}>
                    {ch.enqueued.toLocaleString()}
                  </td>
                  <td style={{ padding: '8px 12px', fontFamily: 'monospace', color: colors.textSecondary }}>
                    {ch.dequeued.toLocaleString()}
                  </td>
                  <td style={{
                    padding: '8px 12px', fontFamily: 'monospace',
                    color: ch.dropped > 0 ? colors.danger : colors.textSecondary,
                    fontWeight: ch.dropped > 0 ? 700 : 400,
                  }}>
                    {ch.dropped.toLocaleString()}
                  </td>
                  <td style={{ padding: '8px 12px' }}>
                    {ch.backpressured ? (
                      <span style={{
                        padding: '2px 8px', borderRadius: 4, fontSize: 11,
                        background: colors.dangerBg, color: colors.danger,
                        border: `1px solid ${colors.dangerBorder}`,
                      }}>ACTIVE</span>
                    ) : (
                      <span style={{
                        padding: '2px 8px', borderRadius: 4, fontSize: 11,
                        color: colors.textMuted,
                      }}>—</span>
                    )}
                    {ch.backpressure_events > 0 && (
                      <span style={{ marginLeft: 6, fontSize: 11, color: colors.textMuted }}>
                        ({ch.backpressure_events}x)
                      </span>
                    )}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>
    </div>
  )
}
