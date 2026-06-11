import { useState } from 'react'
import { colors } from '../../styles/theme'

export interface ProcessEntry {
  pid: number
  comm: string
  cpu_total_pct: number
  cpu_user_pct: number
  cpu_sys_pct: number
  num_threads: number
  rss_kb: number
  state: string
  history?: number[]
}

interface ProcessTableProps {
  processes: ProcessEntry[]
  onSelect?: (pid: number) => void
  selectedPid?: number | null
}

function Sparkline({ values, width = 80, height = 20 }: { values: number[]; width?: number; height?: number }) {
  if (values.length < 2) return null
  const max = Math.max(...values, 1)
  const n = values.length
  const points = values.map((v, i) => {
    const x = (i / (n - 1)) * width
    const y = height - (v / max) * height
    return `${x},${y}`
  }).join(' ')

  return (
    <svg width={width} height={height} style={{ display: 'block' }}>
      <polyline points={points} fill="none" stroke={colors.accent} strokeWidth={1.2} opacity={0.8} />
    </svg>
  )
}

export default function ProcessTable({ processes, onSelect, selectedPid }: ProcessTableProps) {
  const [sortBy, setSortBy] = useState<'cpu' | 'mem'>('cpu')

  const sorted = [...processes].sort((a, b) =>
    sortBy === 'cpu'
      ? b.cpu_total_pct - a.cpu_total_pct
      : b.rss_kb - a.rss_kb
  )

  const thStyle: React.CSSProperties = {
    padding: '8px 10px', textAlign: 'left', fontSize: 11,
    color: colors.textMuted, textTransform: 'uppercase',
    borderBottom: `1px solid ${colors.cardBorder}`, cursor: 'pointer',
  }

  const tdStyle: React.CSSProperties = {
    padding: '8px 10px', fontSize: 13,
    borderBottom: `1px solid ${colors.cardBorder}22`,
  }

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse' }}>
        <thead>
          <tr>
            <th style={thStyle}>#</th>
            <th style={thStyle}>PID</th>
            <th style={thStyle}>Command</th>
            <th style={{ ...thStyle, cursor: 'pointer' }} onClick={() => setSortBy('cpu')}>
              CPU% {sortBy === 'cpu' ? '▼' : ''}
            </th>
            <th style={thStyle}>Trend (30s)</th>
            <th style={thStyle}>User%</th>
            <th style={thStyle}>Sys%</th>
            <th style={thStyle}>Threads</th>
            <th style={{ ...thStyle, cursor: 'pointer' }} onClick={() => setSortBy('mem')}>
              RSS {sortBy === 'mem' ? '▼' : ''}
            </th>
          </tr>
        </thead>
        <tbody>
          {sorted.map((p, idx) => {
            const isSelected = selectedPid === p.pid
            return (
              <tr key={p.pid}
                  onClick={() => onSelect?.(p.pid)}
                  style={{
                    cursor: onSelect ? 'pointer' : 'default',
                    background: isSelected ? colors.activeBg : 'transparent',
                  }}>
                <td style={{ ...tdStyle, color: colors.textMuted, width: 30 }}>{idx + 1}</td>
                <td style={{ ...tdStyle, color: colors.textMuted, fontFamily: 'monospace', width: 60 }}>{p.pid}</td>
                <td style={{ ...tdStyle, color: colors.textPrimary, fontWeight: 500 }}>{p.comm}</td>
                <td style={{ ...tdStyle, color: cpuColor(p.cpu_total_pct), fontWeight: 600, fontFamily: 'monospace' }}>
                  {p.cpu_total_pct.toFixed(1)}%
                </td>
                <td style={tdStyle}>
                  <Sparkline values={p.history ?? [p.cpu_total_pct]} />
                </td>
                <td style={{ ...tdStyle, color: colors.textSecondary, fontFamily: 'monospace' }}>
                  {p.cpu_user_pct.toFixed(1)}%
                </td>
                <td style={{ ...tdStyle, color: colors.textSecondary, fontFamily: 'monospace' }}>
                  {p.cpu_sys_pct.toFixed(1)}%
                </td>
                <td style={{ ...tdStyle, color: colors.textSecondary, textAlign: 'center' }}>{p.num_threads}</td>
                <td style={{ ...tdStyle, color: colors.textSecondary, fontFamily: 'monospace' }}>
                  {formatMem(p.rss_kb)}
                </td>
              </tr>
            )
          })}
          {sorted.length === 0 && (
            <tr>
              <td colSpan={9} style={{ padding: 24, textAlign: 'center', color: colors.textMuted }}>
                No process data yet
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}

function cpuColor(pct: number): string {
  if (pct > 80) return '#ef4444'
  if (pct > 50) return '#f59e0b'
  if (pct > 20) return '#3b82f6'
  return colors.textSecondary
}

function formatMem(kb: number): string {
  if (kb > 1048576) return `${(kb / 1048576).toFixed(1)}G`
  if (kb > 1024) return `${(kb / 1024).toFixed(0)}M`
  return `${kb}K`
}
