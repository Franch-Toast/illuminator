import React, { useState, useCallback, useRef, useEffect, useMemo } from 'react'
import { flamegraph } from 'd3-flame-graph'
import { select } from 'd3-selection'
import 'd3-flame-graph/dist/d3-flamegraph.css'
import { api } from '../services/apiClient'
import { colors } from '../styles/theme'

interface DiffEntry {
  name: string
  baseValue: number
  compValue: number
}

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

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

function flattenNode(node: FlameNode, prefix = ''): DiffEntry[] {
  const name = prefix ? `${prefix} > ${node.name}` : node.name
  const entries: DiffEntry[] = [{ name: node.name, baseValue: node.value, compValue: 0 }]
  if (node.children) {
    for (const ch of node.children) {
      entries.push(...flattenNode(ch, name))
    }
  }
  return entries
}

function aggregateFunctions(node: FlameNode): Map<string, number> {
  const map = new Map<string, number>()
  function walk(n: FlameNode) {
    const existing = map.get(n.name) || 0
    map.set(n.name, existing + n.value)
    if (n.children) for (const ch of n.children) walk(ch)
  }
  walk(node)
  return map
}

function computeDiff(base: FlameNode, comp: FlameNode): DiffEntry[] {
  const baseMap = aggregateFunctions(base)
  const compMap = aggregateFunctions(comp)
  const allNames = new Set([...baseMap.keys(), ...compMap.keys()])
  allNames.delete('root')

  return Array.from(allNames).map(name => ({
    name,
    baseValue: baseMap.get(name) || 0,
    compValue: compMap.get(name) || 0,
  }))
}

type ProfileSource = 'oncpu' | 'offcpu'

const card: React.CSSProperties = {
  background: colors.cardBg, borderRadius: 8, padding: 16,
  border: `1px solid ${colors.cardBorder}`,
}

export default function DiffView() {
  const [baseProfile, setBaseProfile] = useState<FlameNode | null>(null)
  const [compProfile, setCompProfile] = useState<FlameNode | null>(null)
  const [baseSource, setBaseSource] = useState<ProfileSource>('oncpu')
  const [compSource, setCompSource] = useState<ProfileSource>('oncpu')
  const [loading, setLoading] = useState<'base' | 'comp' | null>(null)
  const [sortBy, setSortBy] = useState<'name' | 'diff'>('diff')
  const [filterText, setFilterText] = useState('')
  const baseChartRef = useRef<HTMLDivElement>(null)
  const compChartRef = useRef<HTMLDivElement>(null)

  const loadProfile = useCallback(async (which: 'base' | 'comp') => {
    const source = which === 'base' ? baseSource : compSource
    setLoading(which)
    try {
      const data = source === 'offcpu'
        ? await api.cpuProfileOffcpu()
        : await api.cpuProfileFlamegraph()
      const d = data as any
      if (d.error || !(d.stack_samples?.length > 0)) {
        alert(d.error || 'No samples available')
        setLoading(null)
        return
      }
      const node = convertToFlameNode(d, source === 'offcpu')
      if (which === 'base') setBaseProfile(node)
      else setCompProfile(node)
    } catch (e: any) {
      alert(`Failed: ${e.message}`)
    }
    setLoading(null)
  }, [baseSource, compSource])

  useEffect(() => {
    if (!baseChartRef.current || !baseProfile) return
    baseChartRef.current.innerHTML = ''
    const chart = flamegraph().width(baseChartRef.current.clientWidth || 400)
      .cellHeight(16).minFrameSize(1).inverted(true).selfValue(false)
    select(baseChartRef.current).datum(baseProfile).call(chart as any)
  }, [baseProfile])

  useEffect(() => {
    if (!compChartRef.current || !compProfile) return
    compChartRef.current.innerHTML = ''
    const chart = flamegraph().width(compChartRef.current.clientWidth || 400)
      .cellHeight(16).minFrameSize(1).inverted(true).selfValue(false)
    select(compChartRef.current).datum(compProfile).call(chart as any)
  }, [compProfile])

  const diffData = useMemo(() => {
    if (!baseProfile || !compProfile) return []
    return computeDiff(baseProfile, compProfile)
  }, [baseProfile, compProfile])

  const filtered = useMemo(() => {
    let data = diffData
    if (filterText) {
      const q = filterText.toLowerCase()
      data = data.filter(d => d.name.toLowerCase().includes(q))
    }
    return [...data].sort((a, b) => {
      if (sortBy === 'diff') {
        const da = Math.abs((a.compValue - a.baseValue) / Math.max(a.baseValue, 1))
        const db = Math.abs((b.compValue - b.baseValue) / Math.max(b.baseValue, 1))
        return db - da
      }
      return a.name.localeCompare(b.name)
    }).slice(0, 100)
  }, [diffData, sortBy, filterText])

  const maxVal = useMemo(() =>
    Math.max(1, ...filtered.map(d => Math.max(d.baseValue, d.compValue))),
    [filtered]
  )

  const summary = useMemo(() => {
    if (diffData.length === 0) return null
    const increased = diffData.filter(d => d.compValue > d.baseValue).length
    const decreased = diffData.filter(d => d.compValue < d.baseValue).length
    const newFuncs = diffData.filter(d => d.baseValue === 0 && d.compValue > 0).length
    const removed = diffData.filter(d => d.baseValue > 0 && d.compValue === 0).length
    return { total: diffData.length, increased, decreased, newFuncs, removed }
  }, [diffData])

  const btn = (active: boolean): React.CSSProperties => ({
    padding: '4px 10px', borderRadius: 4, fontSize: 11, cursor: 'pointer',
    border: `1px solid ${active ? colors.accent : colors.cardBorder}`,
    background: active ? colors.activeBg : 'transparent',
    color: active ? colors.accent : colors.textSecondary,
  })

  return (
    <div style={{ padding: 20 }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 20 }}>Compare Profiles</h2>

      {/* Profile Selection */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12, marginBottom: 16 }}>
        <div style={card}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
            <span style={{ fontSize: 13, fontWeight: 600, color: '#3b82f6' }}>Base Profile</span>
            <div style={{ display: 'flex', gap: 4 }}>
              <button style={btn(baseSource === 'oncpu')} onClick={() => setBaseSource('oncpu')}>On-CPU</button>
              <button style={btn(baseSource === 'offcpu')} onClick={() => setBaseSource('offcpu')}>Off-CPU</button>
              <button onClick={() => loadProfile('base')} disabled={loading === 'base'}
                style={{ ...btn(false), background: '#2563eb', color: '#fff', border: 'none' }}>
                {loading === 'base' ? 'Loading...' : 'Capture'}
              </button>
            </div>
          </div>
          <div ref={baseChartRef} style={{ minHeight: 120 }}>
            {!baseProfile && <div style={{ color: '#555', fontSize: 12, padding: 20, textAlign: 'center' }}>Click Capture to load base profile</div>}
          </div>
        </div>
        <div style={card}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
            <span style={{ fontSize: 13, fontWeight: 600, color: '#f59e0b' }}>Compare Profile</span>
            <div style={{ display: 'flex', gap: 4 }}>
              <button style={btn(compSource === 'oncpu')} onClick={() => setCompSource('oncpu')}>On-CPU</button>
              <button style={btn(compSource === 'offcpu')} onClick={() => setCompSource('offcpu')}>Off-CPU</button>
              <button onClick={() => loadProfile('comp')} disabled={loading === 'comp'}
                style={{ ...btn(false), background: '#2563eb', color: '#fff', border: 'none' }}>
                {loading === 'comp' ? 'Loading...' : 'Capture'}
              </button>
            </div>
          </div>
          <div ref={compChartRef} style={{ minHeight: 120 }}>
            {!compProfile && <div style={{ color: '#555', fontSize: 12, padding: 20, textAlign: 'center' }}>Click Capture to load compare profile</div>}
          </div>
        </div>
      </div>

      {/* Diff Summary */}
      {summary && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(5, 1fr)', gap: 12, marginBottom: 16 }}>
          <div style={card}>
            <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase' }}>Total Functions</div>
            <div style={{ fontSize: 22, fontWeight: 700, color: colors.accent }}>{summary.total}</div>
          </div>
          <div style={card}>
            <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase' }}>Increased</div>
            <div style={{ fontSize: 22, fontWeight: 700, color: '#f87171' }}>{summary.increased}</div>
          </div>
          <div style={card}>
            <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase' }}>Decreased</div>
            <div style={{ fontSize: 22, fontWeight: 700, color: '#4ade80' }}>{summary.decreased}</div>
          </div>
          <div style={card}>
            <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase' }}>New</div>
            <div style={{ fontSize: 22, fontWeight: 700, color: '#f59e0b' }}>{summary.newFuncs}</div>
          </div>
          <div style={card}>
            <div style={{ fontSize: 10, color: '#888', textTransform: 'uppercase' }}>Removed</div>
            <div style={{ fontSize: 22, fontWeight: 700, color: '#888' }}>{summary.removed}</div>
          </div>
        </div>
      )}

      {/* Diff Table */}
      <div style={card}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
          <h3 style={{ margin: 0, fontSize: 14 }}>
            Function Diff {filtered.length > 0 ? `(${filtered.length}${diffData.length > 100 ? ` of ${diffData.length}` : ''})` : ''}
          </h3>
          <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
            <input type="text" placeholder="Filter functions..." value={filterText}
              onChange={e => setFilterText(e.target.value)}
              style={{
                background: colors.bg, color: colors.textPrimary,
                border: `1px solid ${colors.cardBorder}`, borderRadius: 4,
                padding: '4px 8px', fontSize: 11, width: 160,
              }} />
            <select value={sortBy} onChange={e => setSortBy(e.target.value as any)}
              style={{
                background: colors.cardBg, color: colors.textPrimary,
                border: `1px solid ${colors.cardBorder}`, borderRadius: 4,
                padding: '4px 8px', fontSize: 11,
              }}>
              <option value="diff">Sort by Change</option>
              <option value="name">Sort by Name</option>
            </select>
          </div>
        </div>

        <div style={{ display: 'flex', gap: 12, marginBottom: 8, fontSize: 11 }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <span style={{ width: 10, height: 10, borderRadius: 2, background: '#3b82f6' }} /> Base
          </span>
          <span style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <span style={{ width: 10, height: 10, borderRadius: 2, background: '#f59e0b' }} /> Compare
          </span>
        </div>

        {filtered.length === 0 && (!baseProfile || !compProfile) ? (
          <div style={{ padding: 32, textAlign: 'center', color: '#555', fontSize: 13 }}>
            Capture both Base and Compare profiles to see the diff analysis.
          </div>
        ) : filtered.length === 0 ? (
          <div style={{ padding: 32, textAlign: 'center', color: '#555', fontSize: 13 }}>
            No matching functions found.
          </div>
        ) : (
          <div>
            <div style={{
              display: 'grid', gridTemplateColumns: '200px 1fr 80px 80px 80px',
              padding: '8px 12px', borderBottom: `1px solid ${colors.cardBorder}`,
              fontSize: 10, color: '#888', fontWeight: 600, textTransform: 'uppercase',
            }}>
              <div>Function</div><div>Comparison</div>
              <div style={{ textAlign: 'right' }}>Base</div>
              <div style={{ textAlign: 'right' }}>Compare</div>
              <div style={{ textAlign: 'right' }}>Change</div>
            </div>
            {filtered.map(d => {
              const diff = d.compValue - d.baseValue
              const pct = d.baseValue > 0 ? ((diff / d.baseValue) * 100) : (d.compValue > 0 ? 100 : 0)
              return (
                <div key={d.name} style={{
                  display: 'grid', gridTemplateColumns: '200px 1fr 80px 80px 80px',
                  padding: '6px 12px', borderBottom: '1px solid #1f2228',
                  alignItems: 'center', fontSize: 12,
                }}>
                  <div style={{ fontFamily: 'monospace', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                    title={d.name}>{d.name}</div>
                  <div style={{ display: 'flex', flexDirection: 'column', gap: 1 }}>
                    <div style={{
                      width: `${(d.baseValue / maxVal) * 100}%`,
                      height: 8, background: '#3b82f6', borderRadius: 2, minWidth: 2,
                    }} />
                    <div style={{
                      width: `${(d.compValue / maxVal) * 100}%`,
                      height: 8, background: '#f59e0b', borderRadius: 2, minWidth: 2,
                    }} />
                  </div>
                  <div style={{ textAlign: 'right', fontFamily: 'monospace', fontSize: 11 }}>{d.baseValue}</div>
                  <div style={{ textAlign: 'right', fontFamily: 'monospace', fontSize: 11 }}>{d.compValue}</div>
                  <div style={{
                    textAlign: 'right', fontWeight: 600, fontSize: 11,
                    color: diff > 0 ? '#f87171' : diff < 0 ? '#4ade80' : '#888',
                  }}>
                    {diff > 0 ? '+' : ''}{pct.toFixed(1)}%
                  </div>
                </div>
              )
            })}
          </div>
        )}
      </div>
    </div>
  )
}
