import React, { useState } from 'react'

interface DiffEntry {
  name: string
  baseValue: number
  compValue: number
}

const DEMO_DIFF: DiffEntry[] = [
  { name: 'main_loop', baseValue: 1200, compValue: 980 },
  { name: 'parse_config', baseValue: 300, compValue: 150 },
  { name: 'disk_read', baseValue: 800, compValue: 850 },
  { name: 'process_data', baseValue: 2500, compValue: 1800 },
  { name: 'network_send', baseValue: 400, compValue: 600 },
  { name: 'alloc_buffer', baseValue: 200, compValue: 50 },
  { name: 'gc_sweep', baseValue: 500, compValue: 700 },
  { name: 'serialize', baseValue: 350, compValue: 200 },
  { name: 'compress', baseValue: 150, compValue: 300 },
  { name: 'log_write', baseValue: 100, compValue: 90 },
]

export default function DiffView() {
  const [data] = useState(DEMO_DIFF)
  const [sortBy, setSortBy] = useState<'name' | 'diff'>('diff')

  const sorted = [...data].sort((a, b) => {
    if (sortBy === 'diff') {
      const da = (a.compValue - a.baseValue) / Math.max(a.baseValue, 1)
      const db = (b.compValue - b.baseValue) / Math.max(b.baseValue, 1)
      return Math.abs(db) - Math.abs(da)
    }
    return a.name.localeCompare(b.name)
  })

  const maxVal = Math.max(...data.map(d => Math.max(d.baseValue, d.compValue)))

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>Diff View</h2>
        <div style={{ display: 'flex', gap: 8 }}>
          <select value={sortBy} onChange={e => setSortBy(e.target.value as any)} style={selectStyle}>
            <option value="diff">Sort by Change</option>
            <option value="name">Sort by Name</option>
          </select>
          <button style={btnStyle}>Load Base Profile</button>
          <button style={btnStyle}>Load Compare Profile</button>
        </div>
      </div>

      <div style={{ display: 'flex', gap: 16, marginBottom: 16, fontSize: 13 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <span style={{ width: 12, height: 12, borderRadius: 2, background: '#3b82f6' }} />
          Base Profile
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <span style={{ width: 12, height: 12, borderRadius: 2, background: '#f59e0b' }} />
          Compare Profile
        </div>
      </div>

      <div style={{ background: '#1a1d23', borderRadius: 8, border: '1px solid #2a2d35' }}>
        <div style={{
          display: 'grid', gridTemplateColumns: '200px 1fr 100px 100px 100px',
          padding: '10px 16px', borderBottom: '1px solid #2a2d35',
          fontSize: 12, color: '#888', fontWeight: 600,
        }}>
          <div>Function</div><div>Comparison</div>
          <div style={{ textAlign: 'right' }}>Base</div>
          <div style={{ textAlign: 'right' }}>Compare</div>
          <div style={{ textAlign: 'right' }}>Change</div>
        </div>

        {sorted.map(d => {
          const diff = d.compValue - d.baseValue
          const pct = d.baseValue > 0 ? ((diff / d.baseValue) * 100) : 0

          return (
            <div key={d.name} style={{
              display: 'grid', gridTemplateColumns: '200px 1fr 100px 100px 100px',
              padding: '8px 16px', borderBottom: '1px solid #1f2228',
              alignItems: 'center', fontSize: 13,
            }}>
              <div style={{ fontFamily: 'monospace' }}>{d.name}</div>
              <div style={{ display: 'flex', gap: 2, alignItems: 'center' }}>
                <div style={{
                  width: `${(d.baseValue / maxVal) * 100}%`,
                  height: 10, background: '#3b82f6', borderRadius: 2, minWidth: 2,
                }} />
                <div style={{
                  width: `${(d.compValue / maxVal) * 100}%`,
                  height: 10, background: '#f59e0b', borderRadius: 2, minWidth: 2,
                  marginTop: 2,
                }} />
              </div>
              <div style={{ textAlign: 'right' }}>{d.baseValue}</div>
              <div style={{ textAlign: 'right' }}>{d.compValue}</div>
              <div style={{
                textAlign: 'right', fontWeight: 600,
                color: diff > 0 ? '#f87171' : diff < 0 ? '#4ade80' : '#888',
              }}>
                {diff > 0 ? '+' : ''}{pct.toFixed(1)}%
              </div>
            </div>
          )
        })}
      </div>
    </div>
  )
}

const selectStyle: React.CSSProperties = {
  background: '#1a1d23', color: '#e0e0e0', border: '1px solid #2a2d35',
  borderRadius: 6, padding: '6px 12px', fontSize: 13,
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '6px 14px', fontSize: 13, cursor: 'pointer',
}
