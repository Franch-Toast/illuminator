import React, { useState, useRef, useEffect } from 'react'

interface TimelineEvent {
  name: string
  category: string
  startMs: number
  durationMs: number
  pid: number
  tid: number
  color: string
}

const DEMO_EVENTS: TimelineEvent[] = [
  { name: 'main_loop', category: 'cpu', startMs: 0, durationMs: 500, pid: 1000, tid: 1, color: '#3b82f6' },
  { name: 'parse_config', category: 'cpu', startMs: 10, durationMs: 45, pid: 1000, tid: 1, color: '#60a5fa' },
  { name: 'disk_read', category: 'io', startMs: 55, durationMs: 120, pid: 1000, tid: 2, color: '#f59e0b' },
  { name: 'process_data', category: 'cpu', startMs: 60, durationMs: 200, pid: 1000, tid: 1, color: '#3b82f6' },
  { name: 'network_send', category: 'net', startMs: 175, durationMs: 80, pid: 1000, tid: 3, color: '#10b981' },
  { name: 'alloc_buffer', category: 'mem', startMs: 100, durationMs: 15, pid: 1000, tid: 1, color: '#a855f7' },
  { name: 'gc_sweep', category: 'mem', startMs: 260, durationMs: 40, pid: 1000, tid: 4, color: '#ef4444' },
  { name: 'write_result', category: 'io', startMs: 300, durationMs: 90, pid: 1000, tid: 2, color: '#f59e0b' },
  { name: 'network_recv', category: 'net', startMs: 350, durationMs: 60, pid: 1000, tid: 3, color: '#10b981' },
  { name: 'cleanup', category: 'cpu', startMs: 420, durationMs: 30, pid: 1000, tid: 1, color: '#60a5fa' },
]

export default function Timeline() {
  const [selected, setSelected] = useState<TimelineEvent | null>(null)
  const [zoom, setZoom] = useState(1)
  const totalMs = 500
  const pxPerMs = (800 / totalMs) * zoom

  const tracks = [...new Set(DEMO_EVENTS.map(e => e.tid))].sort()

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>Timeline View</h2>
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
          <span style={{ fontSize: 13, color: '#888' }}>Zoom:</span>
          <input type="range" min={0.5} max={5} step={0.1} value={zoom}
                 onChange={e => setZoom(Number(e.target.value))}
                 style={{ width: 100 }} />
          <span style={{ fontSize: 13, color: '#888' }}>{zoom.toFixed(1)}x</span>
          <button style={btnStyle}>Import Perfetto</button>
        </div>
      </div>

      <div style={{
        background: '#1a1d23', border: '1px solid #2a2d35',
        borderRadius: 8, overflow: 'auto',
      }}>
        {/* Time ruler */}
        <div style={{ display: 'flex', borderBottom: '1px solid #2a2d35', padding: '4px 80px', gap: 0 }}>
          {Array.from({ length: Math.ceil(totalMs / 50) + 1 }, (_, i) => (
            <div key={i} style={{
              position: 'relative', width: 50 * pxPerMs,
              fontSize: 10, color: '#666', borderLeft: '1px solid #2a2d35',
              paddingLeft: 4,
            }}>
              {i * 50}ms
            </div>
          ))}
        </div>

        {/* Tracks */}
        {tracks.map(tid => {
          const events = DEMO_EVENTS.filter(e => e.tid === tid)
          return (
            <div key={tid} style={{
              display: 'flex', alignItems: 'center',
              borderBottom: '1px solid #1f2228', minHeight: 36,
            }}>
              <div style={{
                width: 80, padding: '0 8px', fontSize: 12,
                color: '#888', flexShrink: 0,
              }}>
                T{tid}
              </div>
              <div style={{ position: 'relative', flex: 1, height: 28 }}>
                {events.map((e, i) => (
                  <div
                    key={i}
                    onClick={() => setSelected(e)}
                    style={{
                      position: 'absolute',
                      left: e.startMs * pxPerMs,
                      width: Math.max(e.durationMs * pxPerMs - 1, 2),
                      height: 22, top: 3,
                      background: e.color, borderRadius: 3,
                      cursor: 'pointer', fontSize: 10,
                      color: '#fff', overflow: 'hidden',
                      padding: '2px 4px', whiteSpace: 'nowrap',
                      opacity: selected && selected !== e ? 0.5 : 1,
                      border: selected === e ? '2px solid #fff' : 'none',
                    }}
                  >
                    {e.durationMs * pxPerMs > 40 ? e.name : ''}
                  </div>
                ))}
              </div>
            </div>
          )
        })}
      </div>

      {selected && (
        <div style={{
          marginTop: 16, background: '#1a1d23', border: '1px solid #2a2d35',
          borderRadius: 8, padding: 16,
        }}>
          <h3 style={{ margin: '0 0 12px', fontSize: 14 }}>Event Details</h3>
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 12, fontSize: 13 }}>
            <div><span style={{ color: '#888' }}>Name:</span> {selected.name}</div>
            <div><span style={{ color: '#888' }}>Category:</span> {selected.category}</div>
            <div><span style={{ color: '#888' }}>Duration:</span> {selected.durationMs}ms</div>
            <div><span style={{ color: '#888' }}>Start:</span> {selected.startMs}ms</div>
            <div><span style={{ color: '#888' }}>PID:</span> {selected.pid}</div>
            <div><span style={{ color: '#888' }}>TID:</span> {selected.tid}</div>
          </div>
        </div>
      )}

      <div style={{ marginTop: 16, display: 'flex', gap: 16, fontSize: 12 }}>
        {['cpu', 'io', 'net', 'mem'].map(cat => (
          <div key={cat} style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <span style={{
              width: 10, height: 10, borderRadius: 2,
              background: cat === 'cpu' ? '#3b82f6' : cat === 'io' ? '#f59e0b'
                        : cat === 'net' ? '#10b981' : '#a855f7',
            }} />
            <span style={{ color: '#888' }}>{cat.toUpperCase()}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '6px 14px', fontSize: 13, cursor: 'pointer',
}
