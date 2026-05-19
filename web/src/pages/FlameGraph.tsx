import React, { useEffect, useRef, useState, useCallback, useMemo } from 'react'
import { flamegraph } from 'd3-flame-graph'
import { select } from 'd3-selection'
import 'd3-flame-graph/dist/d3-flamegraph.css'
import { useTimeStore } from '../stores/useTimeStore'
import { useFilterStore } from '../stores/useFilterStore'
import { timeSeriesStore } from '../services/timeSeriesStore'
import { api } from '../services/apiClient'
import { colors, card as cardStyle } from '../styles/theme'

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

interface SnapshotEntry {
  time: number
  data: FlameNode
  profileType: string
  nodeCount: number
}

const MAX_SNAPSHOTS = 60

function convertToFlameNode(data: any, isOffCpu = false): FlameNode {
  const root: FlameNode = { name: 'root', value: 0, children: [] }
  const samples = data.stack_samples || []
  for (const sample of samples) {
    const frames: string[] = []
    const comm = sample.comm || 'unknown'
    frames.push(comm)
    if (sample.user_stack) {
      for (const f of [...sample.user_stack].reverse()) {
        frames.push(f.function_name?.startsWith('[0x') ? `[${comm}]` : (f.function_name || `[${comm}]`))
      }
    }
    if (sample.kernel_stack) {
      for (const f of [...sample.kernel_stack].reverse()) {
        frames.push(f.function_name || '[kernel]')
      }
    }
    if (frames.length === 1) frames.push('[no stack]')
    const weight = isOffCpu && sample.duration_ns
      ? Math.max(1, Math.round(sample.duration_ns / 1000))
      : (sample.count || 1)
    insertStack(root, frames, weight)
  }
  root.value = root.children?.reduce((s, c) => s + c.value, 0) || 0
  return root
}

function insertStack(node: FlameNode, frames: string[], count: number) {
  if (frames.length === 0) return
  if (!node.children) node.children = []
  let child = node.children.find(c => c.name === frames[0])
  if (!child) {
    child = { name: frames[0]!, value: 0, children: [] }
    node.children.push(child)
  }
  child.value += count
  if (frames.length > 1) insertStack(child, frames.slice(1), count)
}

function countNodes(node: FlameNode): number {
  let c = 1
  if (node.children) for (const ch of node.children) c += countNodes(ch)
  return c
}

export default function FlameGraph() {
  const chartRef = useRef<HTMLDivElement>(null)
  const fgRef = useRef<any>(null)
  const { mode, range, cursor, setCursor } = useTimeStore()
  const { pid } = useFilterStore()

  const [profileType, setProfileType] = useState<'oncpu' | 'offcpu'>('oncpu')
  const [searchText, setSearchText] = useState('')
  const [loading, setLoading] = useState(false)
  const [stub, setStub] = useState(false)
  const [errorMsg, setErrorMsg] = useState('')
  const [snapshots, setSnapshots] = useState<SnapshotEntry[]>([])
  const [activeSnapshot, setActiveSnapshot] = useState<FlameNode | null>(null)
  const [viewingHistorical, setViewingHistorical] = useState(false)

  const loadData = useCallback(async () => {
    setLoading(true)
    try {
      const pipeData = await api.pipelines()
      const pName = profileType === 'offcpu' ? 'offcpu_analysis' : 'cpu_profile'
      const pipeline = (pipeData.pipelines || []).find((p: any) => p.name === pName)
      if (pipeline?.stub) {
        setStub(true)
        setActiveSnapshot(null)
        setErrorMsg('BPF object not configured')
        setLoading(false)
        return
      }
      setStub(false)

      const data = profileType === 'offcpu'
        ? await api.cpuProfileOffcpu()
        : await api.cpuProfileFlamegraph()

      const d = data as any
      if (d.error) {
        setErrorMsg(d.error)
        setActiveSnapshot(null)
      } else if ((d.stack_samples || []).length > 0) {
        const node = convertToFlameNode(d, profileType === 'offcpu')
        setActiveSnapshot(node)
        setErrorMsg('')

        const entry: SnapshotEntry = {
          time: Date.now(), data: node, profileType, nodeCount: countNodes(node),
        }
        setSnapshots(prev => {
          const next = [...prev, entry]
          return next.length > MAX_SNAPSHOTS ? next.slice(-MAX_SNAPSHOTS) : next
        })

        timeSeriesStore.append('profile.samples', Date.now(), {
          count: (d.stack_samples || []).length,
        })
      } else {
        setErrorMsg('No stack samples collected yet')
        setActiveSnapshot(null)
      }
    } catch (e: any) {
      setErrorMsg(e.message)
      setActiveSnapshot(null)
    }
    setLoading(false)
  }, [profileType])

  useEffect(() => {
    if (mode === 'paused') return
    loadData()
    const id = setInterval(loadData, 5000)
    return () => clearInterval(id)
  }, [loadData, mode])

  useEffect(() => {
    if (!chartRef.current || !activeSnapshot) return
    const el = chartRef.current
    el.innerHTML = ''
    const width = el.clientWidth || 900
    const chart = flamegraph()
      .width(width)
      .cellHeight(18)
      .minFrameSize(1)
      .transitionDuration(300)
      .inverted(true)
      .selfValue(false)
    fgRef.current = chart
    select(el).datum(activeSnapshot).call(chart as any)
  }, [activeSnapshot])

  useEffect(() => {
    if (!fgRef.current) return
    if (searchText) fgRef.current.search(searchText)
    else fgRef.current.clear()
  }, [searchText])

  const handleTimelineClick = useCallback((time: number) => {
    const closest = snapshots.reduce<SnapshotEntry | null>((best, s) => {
      if (!best) return s
      return Math.abs(s.time - time) < Math.abs(best.time - time) ? s : best
    }, null)
    if (closest) {
      setActiveSnapshot(closest.data)
      setViewingHistorical(true)
      setCursor(closest.time)
    }
  }, [snapshots, setCursor])

  const handleResume = useCallback(() => {
    setViewingHistorical(false)
    const latest = snapshots[snapshots.length - 1]
    if (latest) setActiveSnapshot(latest.data)
    setCursor(null)
  }, [snapshots, setCursor])

  const handleExportSvg = () => {
    if (!chartRef.current) return
    const svgEl = chartRef.current.querySelector('svg')
    if (!svgEl) return
    const serializer = new XMLSerializer()
    const blob = new Blob([serializer.serializeToString(svgEl)], { type: 'image/svg+xml' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url; a.download = `flamegraph-${profileType}-${Date.now()}.svg`; a.click()
    URL.revokeObjectURL(url)
  }

  const timelineData = useMemo(() =>
    snapshots.filter(s => s.time >= range.start && s.time <= range.end),
    [snapshots, range]
  )

  const card: React.CSSProperties = {
    background: colors.cardBg, borderRadius: 8, padding: 16,
    border: `1px solid ${colors.cardBorder}`,
  }

  return (
    <div style={{ padding: 20 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12, flexWrap: 'wrap', gap: 8 }}>
        <h2 style={{ margin: 0, fontSize: 20 }}>Profiler</h2>
        <div style={{ display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap' }}>
          {viewingHistorical && (
            <button onClick={handleResume} style={{
              padding: '5px 12px', borderRadius: 6, fontSize: 12,
              border: `1px solid ${colors.amber}`, background: 'rgba(245,158,11,0.1)',
              color: colors.amber, cursor: 'pointer',
            }}>
              Back to Live
            </button>
          )}

          <button onClick={() => setProfileType('oncpu')} style={{
            padding: '5px 12px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
            border: `1px solid ${profileType === 'oncpu' ? colors.accent : colors.cardBorder}`,
            background: profileType === 'oncpu' ? colors.activeBg : 'transparent',
            color: profileType === 'oncpu' ? colors.accent : colors.textSecondary,
          }}>On-CPU</button>
          <button onClick={() => setProfileType('offcpu')} style={{
            padding: '5px 12px', borderRadius: 6, fontSize: 12, cursor: 'pointer',
            border: `1px solid ${profileType === 'offcpu' ? colors.accent : colors.cardBorder}`,
            background: profileType === 'offcpu' ? colors.activeBg : 'transparent',
            color: profileType === 'offcpu' ? colors.accent : colors.textSecondary,
          }}>Off-CPU</button>

          <input
            type="text" placeholder="Search functions..."
            value={searchText} onChange={e => setSearchText(e.target.value)}
            style={{
              background: colors.cardBg, color: colors.textPrimary,
              border: `1px solid ${colors.cardBorder}`, borderRadius: 6,
              padding: '5px 10px', fontSize: 12, width: 160,
            }}
          />

          <button onClick={loadData} style={{
            padding: '5px 12px', borderRadius: 6, fontSize: 12,
            background: '#2563eb', color: '#fff', border: 'none', cursor: 'pointer',
          }}>
            {loading ? 'Loading...' : 'Refresh'}
          </button>
          <button onClick={handleExportSvg} style={{
            padding: '5px 12px', borderRadius: 6, fontSize: 12,
            background: '#374151', color: '#fff', border: 'none', cursor: 'pointer',
          }}>Export SVG</button>
        </div>
      </div>

      {/* Snapshot Timeline */}
      {snapshots.length > 1 && (
        <div style={{ ...card, marginBottom: 12, padding: '8px 12px' }}>
          <div style={{ fontSize: 11, color: '#888', marginBottom: 6 }}>
            Profile Timeline ({snapshots.length} snapshots)
          </div>
          <div style={{ display: 'flex', gap: 2, alignItems: 'flex-end', height: 32 }}>
            {timelineData.map((s, i) => (
              <div
                key={i}
                onClick={() => handleTimelineClick(s.time)}
                style={{
                  flex: 1, minWidth: 4, maxWidth: 12,
                  height: `${Math.max(20, Math.min(100, s.nodeCount / 5))}%`,
                  background: cursor && Math.abs(s.time - cursor) < 3000
                    ? colors.accent : 'rgba(59,130,246,0.5)',
                  borderRadius: 2, cursor: 'pointer',
                  transition: 'background 0.15s',
                }}
                title={`${new Date(s.time).toLocaleTimeString()} - ${s.nodeCount} frames`}
              />
            ))}
          </div>
        </div>
      )}

      {/* Flame Graph */}
      <div style={{ ...card, minHeight: 300, overflowX: 'auto' }}>
        <div ref={chartRef} />
        {loading && !activeSnapshot && (
          <div style={{ textAlign: 'center', padding: 40, color: '#888' }}>Loading profile data...</div>
        )}
        {!loading && !activeSnapshot && (
          <div style={{ textAlign: 'center', padding: 48 }}>
            {stub ? (
              <>
                <div style={{ fontSize: 32, marginBottom: 12 }}>🔌</div>
                <div style={{ fontSize: 15, color: '#f59e0b', marginBottom: 8 }}>CPU Profiler is in stub mode</div>
                <div style={{ fontSize: 12, color: '#888', maxWidth: 400, margin: '0 auto' }}>
                  Configure <code style={{ color: '#93c5fd' }}>bpf_object</code> path in pipeline config
                </div>
              </>
            ) : errorMsg ? (
              <>
                <div style={{ fontSize: 32, marginBottom: 12 }}>📊</div>
                <div style={{ fontSize: 13, color: '#888' }}>{errorMsg}</div>
              </>
            ) : (
              <div style={{ fontSize: 13, color: '#888' }}>Waiting for samples...</div>
            )}
          </div>
        )}
      </div>

      <div style={{ marginTop: 8, display: 'flex', justifyContent: 'space-between', fontSize: 11, color: '#555' }}>
        <span>Hover to inspect. Click to zoom. Search to highlight.</span>
        <span>{activeSnapshot ? `${countNodes(activeSnapshot)} unique frames` : ''}</span>
      </div>
    </div>
  )
}
