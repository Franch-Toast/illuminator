import { colors } from '../../styles/theme'

export interface CpuSummary {
  avgLoad: number
  maxCore: { name: string; pct: number }
  ctxSwitches: number
  runQueue: number
}

interface SummaryCardsProps {
  data: CpuSummary | null
}

export default function SummaryCards({ data }: SummaryCardsProps) {
  const card: React.CSSProperties = {
    background: colors.bg, borderRadius: 6, padding: '12px 16px',
    border: `1px solid ${colors.cardBorder}`,
  }

  if (!data) {
    return (
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 12 }}>
        {Array.from({ length: 4 }).map((_, i) => (
          <div key={i} style={card}>
            <div style={{ fontSize: 11, color: colors.textMuted }}>---</div>
            <div style={{ fontSize: 18, color: colors.textMuted }}>--</div>
          </div>
        ))}
      </div>
    )
  }

  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(150px, 1fr))', gap: 12 }}>
      <div style={card}>
        <div style={{ fontSize: 10, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 4 }}>
          Avg CPU
        </div>
        <div style={{ fontSize: 20, fontWeight: 700, color: cpuColor(data.avgLoad), fontFamily: 'monospace' }}>
          {data.avgLoad.toFixed(1)}%
        </div>
      </div>
      <div style={card}>
        <div style={{ fontSize: 10, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 4 }}>
          Hottest Core
        </div>
        <div style={{ fontSize: 20, fontWeight: 700, color: cpuColor(data.maxCore.pct), fontFamily: 'monospace' }}>
          {data.maxCore.pct.toFixed(0)}%
        </div>
        <div style={{ fontSize: 10, color: colors.textMuted }}>{data.maxCore.name}</div>
      </div>
      <div style={card}>
        <div style={{ fontSize: 10, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 4 }}>
          Context Sw/s
        </div>
        <div style={{ fontSize: 20, fontWeight: 700, color: colors.textPrimary, fontFamily: 'monospace' }}>
          {formatNum(data.ctxSwitches)}
        </div>
      </div>
      <div style={card}>
        <div style={{ fontSize: 10, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 4 }}>
          Run Queue
        </div>
        <div style={{ fontSize: 20, fontWeight: 700, color: colors.textPrimary, fontFamily: 'monospace' }}>
          {data.runQueue}
        </div>
      </div>
    </div>
  )
}

function cpuColor(pct: number): string {
  if (pct > 80) return '#ef4444'
  if (pct > 50) return '#f59e0b'
  return colors.success
}

function formatNum(n: number): string {
  if (n > 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`
  if (n > 1_000) return `${(n / 1_000).toFixed(1)}k`
  return String(Math.round(n))
}
