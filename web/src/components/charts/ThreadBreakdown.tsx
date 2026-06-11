import { colors } from '../../styles/theme'

export interface ThreadEntry {
  tid: number
  comm: string
  cpu_total_pct: number
  cpu_user_pct: number
  cpu_sys_pct: number
  state: string
}

interface ThreadBreakdownProps {
  threads: ThreadEntry[]
  processComm: string
}

function MiniSparkline({ value, max = 100 }: { value: number; max?: number }) {
  const pct = Math.min(100, (value / max) * 100)
  return (
    <div style={{ width: 80, height: 12, background: colors.bg, borderRadius: 2, overflow: 'hidden' }}>
      <div style={{
        width: `${pct}%`, height: '100%',
        background: value > 50 ? '#ef4444' : value > 20 ? '#f59e0b' : '#3b82f6',
        borderRadius: 2, transition: 'width 0.3s',
      }} />
    </div>
  )
}

export default function ThreadBreakdown({ threads, processComm }: ThreadBreakdownProps) {
  if (threads.length === 0) {
    return (
      <div style={{ padding: 16, textAlign: 'center', color: colors.textMuted, fontSize: 12 }}>
        No thread data available. Threads are shown when the process CPU exceeds the threshold.
      </div>
    )
  }

  const sorted = [...threads].sort((a, b) => b.cpu_total_pct - a.cpu_total_pct)

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
            <th style={th}>TID</th>
            <th style={th}>Name</th>
            <th style={th}>CPU%</th>
            <th style={th}>Bar</th>
            <th style={th}>User%</th>
            <th style={th}>Sys%</th>
            <th style={th}>State</th>
          </tr>
        </thead>
        <tbody>
          {sorted.map(t => (
            <tr key={t.tid} style={{ borderBottom: `1px solid ${colors.cardBorder}22` }}>
              <td style={{ ...td, color: colors.textMuted, fontFamily: 'monospace' }}>{t.tid}</td>
              <td style={{ ...td, color: colors.textPrimary, fontWeight: 500 }}>
                {t.comm || processComm}
              </td>
              <td style={{ ...td, color: cpuColor(t.cpu_total_pct), fontWeight: 600, fontFamily: 'monospace' }}>
                {t.cpu_total_pct.toFixed(1)}%
              </td>
              <td style={td}><MiniSparkline value={t.cpu_total_pct} /></td>
              <td style={{ ...td, color: colors.textSecondary, fontFamily: 'monospace' }}>
                {t.cpu_user_pct.toFixed(1)}%
              </td>
              <td style={{ ...td, color: colors.textSecondary, fontFamily: 'monospace' }}>
                {t.cpu_sys_pct.toFixed(1)}%
              </td>
              <td style={{ ...td, color: stateColor(t.state) }}>{t.state}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

const th: React.CSSProperties = {
  padding: '6px 8px', textAlign: 'left', fontSize: 10,
  color: colors.textMuted, textTransform: 'uppercase',
}

const td: React.CSSProperties = {
  padding: '6px 8px',
}

function cpuColor(pct: number): string {
  if (pct > 50) return '#ef4444'
  if (pct > 20) return '#f59e0b'
  if (pct > 5) return '#3b82f6'
  return colors.textSecondary
}

function stateColor(state: string): string {
  switch (state) {
    case 'R': return colors.success
    case 'S': return colors.textMuted
    case 'D': return '#ef4444'
    default: return colors.textSecondary
  }
}
