import React, { useEffect, useRef, useState } from 'react'

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

const COLORS = [
  '#f97316', '#ef4444', '#eab308', '#f59e0b',
  '#d97706', '#dc2626', '#ea580c', '#ca8a04',
]

function hashColor(name: string): string {
  let hash = 0
  for (let i = 0; i < name.length; i++) {
    hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0
  }
  return COLORS[Math.abs(hash) % COLORS.length]
}

function FlameBar({ node, depth, x, width, totalValue, onHover, maxDepth }: {
  node: FlameNode; depth: number; x: number; width: number
  totalValue: number; onHover: (n: FlameNode | null) => void; maxDepth: number
}) {
  if (width < 1 || depth > maxDepth) return null
  const h = 20
  const y = depth * (h + 1)
  let childX = x

  return (
    <g>
      <rect
        x={x} y={y} width={Math.max(width - 0.5, 0.5)} height={h}
        fill={hashColor(node.name)} rx={2}
        style={{ cursor: 'pointer' }}
        onMouseEnter={() => onHover(node)}
        onMouseLeave={() => onHover(null)}
      />
      {width > 40 && (
        <text x={x + 4} y={y + 14} fontSize={11} fill="#fff"
              style={{ pointerEvents: 'none' }}>
          {node.name.length > width / 7 ? node.name.slice(0, Math.floor(width / 7)) + '…' : node.name}
        </text>
      )}
      {node.children?.map((child, i) => {
        const childW = (child.value / totalValue) * width
        const el = (
          <FlameBar key={i} node={child} depth={depth + 1}
                    x={childX} width={childW}
                    totalValue={node.value} onHover={onHover}
                    maxDepth={maxDepth} />
        )
        childX += childW
        return el
      })}
    </g>
  )
}

const DEMO_DATA: FlameNode = {
  name: 'root', value: 100,
  children: [
    { name: 'main', value: 80, children: [
      { name: 'process_request', value: 50, children: [
        { name: 'parse_input', value: 15 },
        { name: 'compute', value: 25, children: [
          { name: 'matrix_multiply', value: 18 },
          { name: 'normalize', value: 7 },
        ]},
        { name: 'serialize_output', value: 10 },
      ]},
      { name: 'handle_io', value: 20, children: [
        { name: 'read_socket', value: 12 },
        { name: 'write_socket', value: 8 },
      ]},
      { name: 'gc_collect', value: 10 },
    ]},
    { name: 'idle_thread', value: 15 },
    { name: 'signal_handler', value: 5 },
  ]
}

export default function FlameGraph() {
  const [hovered, setHovered] = useState<FlameNode | null>(null)
  const [data] = useState<FlameNode>(DEMO_DATA)
  const svgWidth = 900
  const maxDepth = 15

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>Flame Graph</h2>
        <div style={{ display: 'flex', gap: 8 }}>
          <select style={selectStyle}>
            <option>CPU Profile</option>
            <option>Memory Allocations</option>
            <option>I/O Latency</option>
          </select>
          <button style={btnStyle}>Import pprof</button>
          <button style={btnStyle}>Export SVG</button>
        </div>
      </div>

      {hovered && (
        <div style={{
          background: '#1a1d23', border: '1px solid #2a2d35', borderRadius: 6,
          padding: '8px 14px', marginBottom: 12, fontSize: 13,
        }}>
          <strong>{hovered.name}</strong> — {hovered.value} samples
          ({((hovered.value / data.value) * 100).toFixed(1)}% of total)
        </div>
      )}

      <div style={{
        background: '#1a1d23', border: '1px solid #2a2d35',
        borderRadius: 8, padding: 16, overflowX: 'auto',
      }}>
        <svg width={svgWidth} height={maxDepth * 21 + 20}>
          <FlameBar node={data} depth={0} x={0} width={svgWidth}
                    totalValue={data.value} onHover={setHovered}
                    maxDepth={maxDepth} />
        </svg>
      </div>

      <div style={{ marginTop: 16, fontSize: 12, color: '#666' }}>
        Hover over frames to see details. Click to zoom.
        Import pprof/folded-stack files for real profiling data.
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
