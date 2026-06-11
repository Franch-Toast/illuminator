import { useMemo } from 'react'
import { colors } from '../../styles/theme'

export interface CpuDataPoint {
  timestamp: number
  user_pct: number
  system_pct: number
  irq_pct: number
  softirq_pct: number
  iowait_pct: number
  steal_pct: number
  idle_pct: number
}

interface StackedAreaChartProps {
  data: CpuDataPoint[]
  width?: number
  height?: number
}

const LAYERS: { key: keyof Omit<CpuDataPoint, 'timestamp'>; color: string; label: string }[] = [
  { key: 'steal_pct', color: '#9333ea', label: 'Steal' },
  { key: 'softirq_pct', color: '#ec4899', label: 'SoftIRQ' },
  { key: 'irq_pct', color: '#f97316', label: 'IRQ' },
  { key: 'iowait_pct', color: '#eab308', label: 'IOWait' },
  { key: 'system_pct', color: '#ef4444', label: 'System' },
  { key: 'user_pct', color: '#3b82f6', label: 'User' },
  { key: 'idle_pct', color: '#374151', label: 'Idle' },
]

export default function StackedAreaChart({ data, width = 700, height = 220 }: StackedAreaChartProps) {
  const padding = { top: 10, right: 20, bottom: 24, left: 40 }
  const innerW = width - padding.left - padding.right
  const innerH = height - padding.top - padding.bottom

  const paths = useMemo(() => {
    if (data.length < 2) return []

    const n = data.length
    const xScale = (i: number) => padding.left + (i / (n - 1)) * innerW

    const stackedLayers: { key: string; color: string; d: string }[] = []
    const cumulative = Array(n).fill(0)

    for (const layer of LAYERS) {
      const prevY = [...cumulative]
      for (let i = 0; i < n; i++) {
        cumulative[i] += (data[i][layer.key] as number) || 0
      }

      const yScale = (v: number) => padding.top + innerH - (v / 100) * innerH

      let d = `M ${xScale(0)} ${yScale(cumulative[0])}`
      for (let i = 1; i < n; i++) {
        d += ` L ${xScale(i)} ${yScale(cumulative[i])}`
      }
      for (let i = n - 1; i >= 0; i--) {
        d += ` L ${xScale(i)} ${yScale(prevY[i])}`
      }
      d += ' Z'
      stackedLayers.push({ key: layer.key, color: layer.color, d })
    }
    return stackedLayers
  }, [data, innerW, innerH, padding.left, padding.top])

  const yTicks = [0, 25, 50, 75, 100]

  return (
    <div>
      <svg width={width} height={height} style={{ display: 'block' }}>
        {yTicks.map(v => {
          const y = padding.top + innerH - (v / 100) * innerH
          return (
            <g key={v}>
              <line x1={padding.left} x2={width - padding.right}
                    y1={y} y2={y} stroke={colors.cardBorder} strokeWidth={0.5} />
              <text x={padding.left - 6} y={y + 4} textAnchor="end"
                    fill={colors.textMuted} fontSize={10}>{v}%</text>
            </g>
          )
        })}

        {paths.map(p => (
          <path key={p.key} d={p.d} fill={p.color} opacity={0.85} />
        ))}
      </svg>

      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap', paddingLeft: padding.left }}>
        {LAYERS.filter(l => l.key !== 'idle_pct').map(l => (
          <div key={l.key} style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <div style={{ width: 10, height: 10, borderRadius: 2, background: l.color }} />
            <span style={{ fontSize: 11, color: colors.textSecondary }}>{l.label}</span>
          </div>
        ))}
      </div>
    </div>
  )
}
