import React, { useMemo, useCallback, useEffect, useState, useRef } from 'react'
import {
  ResponsiveContainer, AreaChart, Area, XAxis, YAxis,
  CartesianGrid, Tooltip, Legend,
} from 'recharts'
import { useTimeStore } from '../stores/useTimeStore'
import { usePipelineStore } from '../stores/usePipelineStore'
import { timeSeriesStore, type DataPoint } from '../services/timeSeriesStore'
import { api } from '../services/apiClient'
import { colors } from '../styles/theme'

const card: React.CSSProperties = {
  background: colors.cardBg, borderRadius: 8, padding: 20,
  border: `1px solid ${colors.cardBorder}`,
}

const CPU_COLORS = {
  user: '#3b82f6', system: '#ef4444', iowait: '#f59e0b',
  irq: '#a855f7', steal: '#6b7280',
}

function busyHeatColor(busyPct: number): string {
  const x = Math.max(0, Math.min(100, busyPct)) / 100
  if (x < 0.5) {
    const t = x * 2
    return `rgb(${Math.round(34 + 200 * t)},${Math.round(197 - 18 * t)},${Math.round(94 - 86 * t)})`
  }
  const t = (x - 0.5) * 2
  return `rgb(${Math.round(234 + 5 * t)},${Math.round(179 - 111 * t)},${Math.round(8 + 60 * t)})`
}

function coreSortKey(cpu: string): number {
  const m = cpu.match(/\d+/g)
  return m?.length ? parseInt(m[m.length - 1]!, 10) : 0
}

interface CpuCoreMetrics {
  cpu: string; type: string; busy_pct: number
  user_pct: number; system_pct: number; nice_pct: number
  idle_pct: number; iowait_pct: number; irq_pct: number
  softirq_pct: number; steal_pct: number
  user_pct_ema?: number; system_pct_ema?: number; busy_pct_ema?: number
}

function normalizeLabels(raw: unknown): Record<string, string> {
  if (!raw) return {}
  if (Array.isArray(raw)) {
    const out: Record<string, string> = {}
    for (const item of raw) {
      if (item && typeof item === 'object') {
        const o = item as Record<string, unknown>
        const key = o.key ?? o.name
        const val = o.value ?? o.val
        if (key != null) out[String(key)] = val != null ? String(val) : ''
      }
    }
    return out
  }
  if (typeof raw === 'object') {
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(raw as Record<string, unknown>)) {
      out[k] = v != null ? String(v) : ''
    }
    return out
  }
  return {}
}

function parseUtilization(data: unknown) {
  const result = {
    cores: [] as CpuCoreMetrics[],
    total: null as CpuCoreMetrics | null,
    counters: null as { context_switches_per_sec: number; interrupts_per_sec: number } | null,
    loadavg: null as { load_1m: number; load_5m: number; load_15m: number } | null,
    runqueue: null as { procs_running: number; procs_blocked: number } | null,
  }
  const d = data as { records?: Array<{ labels?: Record<string, string>; fields?: Record<string, number> }> } | null
  if (!d?.records) return result

  for (const rec of d.records) {
    const labels = normalizeLabels(rec.labels)
    const fields = rec.fields || {}
    const type = labels.type || ''

    if (type === 'cpu_total' || type === 'cpu_core') {
      const core: CpuCoreMetrics = {
        cpu: labels.cpu || '', type,
        user_pct: fields.user_pct ?? 0, system_pct: fields.system_pct ?? 0,
        nice_pct: fields.nice_pct ?? 0, idle_pct: fields.idle_pct ?? 0,
        iowait_pct: fields.iowait_pct ?? 0, irq_pct: fields.irq_pct ?? 0,
        softirq_pct: fields.softirq_pct ?? 0, steal_pct: fields.steal_pct ?? 0,
        busy_pct: fields.busy_pct ?? 0,
        user_pct_ema: fields.user_pct_ema, system_pct_ema: fields.system_pct_ema,
        busy_pct_ema: fields.busy_pct_ema,
      }
      if (type === 'cpu_total') result.total = core
      else result.cores.push(core)
    } else if (type === 'system_counters') {
      result.counters = {
        context_switches_per_sec: fields.context_switches_per_sec ?? 0,
        interrupts_per_sec: fields.interrupts_per_sec ?? 0,
      }
    } else if (type === 'loadavg') {
      result.loadavg = {
        load_1m: fields.load_1m ?? 0, load_5m: fields.load_5m ?? 0,
        load_15m: fields.load_15m ?? 0,
      }
    } else if (type === 'runqueue') {
      result.runqueue = {
        procs_running: fields.procs_running ?? 0,
        procs_blocked: fields.procs_blocked ?? 0,
      }
    }
  }
  return result
}

export default function CpuOverview() {
  const { mode, range } = useTimeStore()
  const [data, setData] = useState<ReturnType<typeof parseUtilization>>({
    cores: [], total: null, counters: null, loadavg: null, runqueue: null,
  })
  const [tsData, setTsData] = useState<DataPoint[]>([])
  const [error, setError] = useState<string | null>(null)
  const intervalRef = useRef<ReturnType<typeof setInterval>>()

  const fetchData = useCallback(async () => {
    try {
      const json = await api.cpuUtilization()
      const parsed = parseUtilization(json)
      setData(parsed)
      setError(null)
      if (parsed.total) {
        const now = Date.now()
        timeSeriesStore.append('cpu.total', now, {
          user: parsed.total.user_pct,
          system: parsed.total.system_pct,
          iowait: parsed.total.iowait_pct,
          irq: parsed.total.irq_pct + parsed.total.softirq_pct,
          steal: parsed.total.steal_pct,
          idle: parsed.total.idle_pct,
        })
        for (const core of parsed.cores) {
          timeSeriesStore.append(`cpu.core.${core.cpu}`, now, {
            busy: core.busy_pct,
          })
        }
      }
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    }
  }, [])

  useEffect(() => {
    if (mode === 'paused') return
    const initTimer = window.setTimeout(fetchData, 0)
    intervalRef.current = setInterval(fetchData, 1000)
    return () => {
      clearTimeout(initTimer)
      clearInterval(intervalRef.current)
    }
  }, [fetchData, mode])

  useEffect(() => {
    const update = () => setTsData(timeSeriesStore.query('cpu.total', range.start, range.end))
    update()
    return timeSeriesStore.subscribe('cpu.total', update)
  }, [range.start, range.end])

  const sortedCores = useMemo(
    () => [...data.cores].sort((a, b) => coreSortKey(a.cpu) - coreSortKey(b.cpu)),
    [data.cores]
  )

  const heatmapCols = Math.min(16, Math.max(4, Math.ceil(Math.sqrt(sortedCores.length))))

  return (
    <div style={{ padding: 20 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 }}>
        <h2 style={{ margin: 0, fontSize: 20 }}>Dashboard</h2>
        {mode === 'paused' && (
          <span style={{ fontSize: 12, color: colors.amber, padding: '2px 10px',
            border: `1px solid ${colors.amber}`, borderRadius: 4 }}>
            Viewing historical data
          </span>
        )}
      </div>

      {error && (
        <div style={{ ...card, marginBottom: 16, borderColor: '#7f1d1d', color: '#f87171', fontSize: 13 }}>
          {error}
        </div>
      )}

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 16, marginBottom: 16 }}>
        {/* CPU Utilization Area Chart */}
        <div style={{ ...card, gridColumn: '1 / -1' }}>
          <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>CPU Utilization</h3>
          <div style={{ width: '100%', height: 280 }}>
            <ResponsiveContainer width="100%" height="100%">
              <AreaChart data={tsData} margin={{ top: 8, right: 12, left: 0, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#2a2d35" />
                <XAxis dataKey="time" type="number" domain={['dataMin', 'dataMax']}
                  tickFormatter={ts => new Date(ts).toLocaleTimeString()}
                  stroke="#555" tick={{ fill: '#888', fontSize: 10 }} />
                <YAxis domain={[0, 100]} stroke="#555" tick={{ fill: '#888', fontSize: 10 }}
                  tickFormatter={v => `${v}%`} />
                <Tooltip contentStyle={{ background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 8, fontSize: 12 }}
                  labelFormatter={ts => new Date(ts as number).toLocaleString()}
                  formatter={(v: number) => [`${v.toFixed(1)}%`, '']} />
                <Legend wrapperStyle={{ fontSize: 11 }} />
                <Area type="monotone" dataKey="user" name="User" stackId="1" stroke={CPU_COLORS.user} fill={CPU_COLORS.user} fillOpacity={0.85} />
                <Area type="monotone" dataKey="system" name="System" stackId="1" stroke={CPU_COLORS.system} fill={CPU_COLORS.system} fillOpacity={0.85} />
                <Area type="monotone" dataKey="iowait" name="IO Wait" stackId="1" stroke={CPU_COLORS.iowait} fill={CPU_COLORS.iowait} fillOpacity={0.85} />
                <Area type="monotone" dataKey="irq" name="IRQ" stackId="1" stroke={CPU_COLORS.irq} fill={CPU_COLORS.irq} fillOpacity={0.85} />
                <Area type="monotone" dataKey="steal" name="Steal" stackId="1" stroke={CPU_COLORS.steal} fill={CPU_COLORS.steal} fillOpacity={0.85} />
              </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>

      {/* CPU Core Heatmap */}
      <div style={{ ...card, marginBottom: 16 }}>
        <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>Per-core Heatmap</h3>
        {sortedCores.length === 0 ? (
          <p style={{ margin: 0, color: '#666', fontSize: 13 }}>No core metrics yet.</p>
        ) : (
          <div style={{
            display: 'grid',
            gridTemplateColumns: `repeat(${heatmapCols}, 1fr)`,
            gap: 4,
          }}>
            {sortedCores.map((core, i) => (
              <div key={`${core.cpu}-${i}`}
                style={{
                  background: busyHeatColor(core.busy_pct),
                  borderRadius: 4, padding: '6px 4px',
                  textAlign: 'center', fontSize: 10,
                  color: core.busy_pct > 60 ? '#fff' : '#222',
                  fontWeight: 600, cursor: 'default',
                  minHeight: 36, display: 'flex', flexDirection: 'column',
                  alignItems: 'center', justifyContent: 'center',
                }}
                title={`${core.cpu}: ${core.busy_pct.toFixed(1)}% busy`}
              >
                <div>{core.cpu}</div>
                <div>{core.busy_pct.toFixed(0)}%</div>
              </div>
            ))}
          </div>
        )}
        <div style={{ display: 'flex', gap: 4, marginTop: 8, alignItems: 'center', fontSize: 10, color: '#888' }}>
          <span>0%</span>
          <div style={{
            flex: 1, height: 8, borderRadius: 4,
            background: 'linear-gradient(to right, rgb(34,197,94), rgb(234,179,8), rgb(239,68,68))',
          }} />
          <span>100%</span>
        </div>
      </div>

      {/* CPU Core Heatmap Timeline */}
      {sortedCores.length > 0 && (
        <CoreHeatmapTimeline coreNames={sortedCores.map(c => c.cpu)} range={range} />
      )}

      {/* Stats Cards */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(160px, 1fr))', gap: 12, marginBottom: 16 }}>
        <StatCard label="Load 1m" value={data.loadavg?.load_1m.toFixed(2)} />
        <StatCard label="Load 5m" value={data.loadavg?.load_5m.toFixed(2)} />
        <StatCard label="Load 15m" value={data.loadavg?.load_15m.toFixed(2)} />
        <StatCard label="Ctx Switches/s"
          value={data.counters ? Math.round(data.counters.context_switches_per_sec).toLocaleString() : undefined}
          small />
        <StatCard label="Interrupts/s"
          value={data.counters ? Math.round(data.counters.interrupts_per_sec).toLocaleString() : undefined}
          small />
        <StatCard label="Running / Blocked"
          value={data.runqueue ? `${data.runqueue.procs_running} / ${data.runqueue.procs_blocked}` : undefined}
          small />
      </div>

      {/* Pipeline Health */}
      <PipelineHealth />
    </div>
  )
}

function StatCard({ label, value, small }: { label: string; value?: string; small?: boolean }) {
  return (
    <div style={card}>
      <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase', letterSpacing: '0.05em', marginBottom: 6 }}>
        {label}
      </div>
      <div style={{ fontSize: small ? 20 : 24, fontWeight: 700, color: value ? colors.accent : '#555' }}>
        {value ?? '—'}
      </div>
    </div>
  )
}

function CoreHeatmapTimeline({ coreNames, range }: {
  coreNames: string[]
  range: { start: number; end: number }
}) {
  const [heatData, setHeatData] = useState<Map<string, DataPoint[]>>(new Map())

  useEffect(() => {
    const update = () => {
      const map = new Map<string, DataPoint[]>()
      for (const name of coreNames) {
        map.set(name, timeSeriesStore.query(`cpu.core.${name}`, range.start, range.end))
      }
      setHeatData(map)
    }
    update()
    const unsubs = coreNames.map(n =>
      timeSeriesStore.subscribe(`cpu.core.${n}`, update)
    )
    return () => unsubs.forEach(fn => fn())
  }, [coreNames, range.start, range.end])

  const maxCols = 60
  const allTimes = new Set<number>()
  for (const [, points] of heatData) {
    for (const p of points) allTimes.add(p.time)
  }
  const sortedTimes = Array.from(allTimes).sort((a, b) => a - b)
  const step = Math.max(1, Math.ceil(sortedTimes.length / maxCols))
  const sampledTimes = sortedTimes.filter((_, i) => i % step === 0)

  if (sampledTimes.length < 2) return null

  const cellW = Math.max(4, Math.min(14, Math.floor(600 / sampledTimes.length)))
  const cellH = 14
  const labelW = 50
  const svgW = labelW + sampledTimes.length * (cellW + 1)
  const svgH = coreNames.length * (cellH + 1) + 20

  return (
    <div style={{ ...card, marginBottom: 16 }}>
      <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>Core Activity Timeline</h3>
      <div style={{ overflowX: 'auto' }}>
        <svg width={svgW} height={svgH} style={{ display: 'block' }}>
          {coreNames.map((name, row) => {
            const points = heatData.get(name) || []
            const byTime = new Map<number, number>()
            for (const p of points) byTime.set(p.time, p.busy ?? 0)

            return (
              <g key={name}>
                <text x={0} y={row * (cellH + 1) + cellH - 2} fill="#888"
                  fontSize={9} fontFamily="monospace">{name}</text>
                {sampledTimes.map((t, col) => {
                  let closest = 0
                  let minDist = Infinity
                  for (const p of points) {
                    const d = Math.abs(p.time - t)
                    if (d < minDist) { minDist = d; closest = p.busy ?? 0 }
                  }
                  return (
                    <rect key={col}
                      x={labelW + col * (cellW + 1)}
                      y={row * (cellH + 1)}
                      width={cellW} height={cellH}
                      rx={2}
                      fill={busyHeatColor(closest)}
                      opacity={0.9}
                    >
                      <title>{name} @ {new Date(t).toLocaleTimeString()}: {closest.toFixed(1)}%</title>
                    </rect>
                  )
                })}
              </g>
            )
          })}
          {/* Time axis */}
          {sampledTimes.filter((_, i) => i % Math.max(1, Math.floor(sampledTimes.length / 6)) === 0).map((t, i) => {
            const col = sampledTimes.indexOf(t)
            return (
              <text key={i}
                x={labelW + col * (cellW + 1)}
                y={svgH - 2}
                fill="#666" fontSize={8}
              >
                {new Date(t).toLocaleTimeString(undefined, { minute: '2-digit', second: '2-digit' })}
              </text>
            )
          })}
        </svg>
      </div>
    </div>
  )
}

function PipelineHealth() {
  const { pipelines } = usePipelineStore()
  if (pipelines.length === 0) return null

  return (
    <div style={card}>
      <h3 style={{ margin: '0 0 12px', fontSize: 14, fontWeight: 600 }}>Pipeline Health</h3>
      <div style={{ overflowX: 'auto' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
          <thead>
            <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              {['Pipeline', 'Status', 'Queue', 'Throughput', 'Drops', 'Backpressure'].map(h => (
                <th key={h} style={{
                  padding: '6px 10px', textAlign: 'left', color: '#888',
                  fontWeight: 500, fontSize: 10, textTransform: 'uppercase',
                }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {pipelines.map(p => {
              const ch = p.channel
              const utilPct = ch && ch.capacity > 0 ? (ch.size / ch.capacity) * 100 : 0
              const barColor = utilPct > 80 ? colors.danger : utilPct > 50 ? colors.amber : colors.success
              return (
                <tr key={p.name} style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
                  <td style={{ padding: '6px 10px', fontWeight: 600, fontSize: 12 }}>{p.name}</td>
                  <td style={{ padding: '6px 10px' }}>
                    <span style={{
                      padding: '2px 8px', borderRadius: 4, fontSize: 10,
                      background: p.running ? 'rgba(74,222,128,0.15)' : 'rgba(107,114,128,0.15)',
                      color: p.running ? '#4ade80' : '#888',
                    }}>
                      {p.running ? 'RUNNING' : p.stub ? 'STUB' : 'STOPPED'}
                    </span>
                  </td>
                  <td style={{ padding: '6px 10px' }}>
                    {ch && (
                      <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                        <div style={{ width: 60, height: 5, background: colors.cardBorder, borderRadius: 3 }}>
                          <div style={{
                            width: `${Math.min(100, utilPct)}%`, height: '100%',
                            background: barColor, borderRadius: 3, transition: 'width 0.3s',
                          }} />
                        </div>
                        <span style={{ fontSize: 10, color: '#888' }}>{utilPct.toFixed(0)}%</span>
                      </div>
                    )}
                  </td>
                  <td style={{ padding: '6px 10px', fontFamily: 'monospace', fontSize: 11, color: '#b0b0b0' }}>
                    {ch ? `${ch.enqueued.toLocaleString()} / ${ch.dequeued.toLocaleString()}` : '—'}
                  </td>
                  <td style={{
                    padding: '6px 10px', fontFamily: 'monospace', fontSize: 11,
                    color: ch && ch.dropped > 0 ? colors.danger : '#888',
                    fontWeight: ch && ch.dropped > 0 ? 700 : 400,
                  }}>
                    {ch ? ch.dropped.toLocaleString() : '—'}
                  </td>
                  <td style={{ padding: '6px 10px' }}>
                    {ch?.backpressured ? (
                      <span style={{
                        padding: '2px 6px', borderRadius: 4, fontSize: 10,
                        background: 'rgba(248,113,113,0.15)', color: '#f87171',
                      }}>ACTIVE ({ch.backpressure_events}x)</span>
                    ) : (
                      <span style={{ fontSize: 10, color: '#555' }}>
                        {ch && ch.backpressure_events > 0 ? `${ch.backpressure_events}x` : '—'}
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
