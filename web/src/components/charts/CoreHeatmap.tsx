import { useMemo } from 'react'
import { colors } from '../../styles/theme'

export interface CoreDataPoint {
  timestamp: number
  cores: { name: string; busy_pct: number }[]
}

interface CoreHeatmapProps {
  data: CoreDataPoint[]
  width?: number
}

function busyColor(pct: number): string {
  if (pct < 20) return '#064e3b'
  if (pct < 40) return '#065f46'
  if (pct < 60) return '#ca8a04'
  if (pct < 80) return '#d97706'
  return '#dc2626'
}

export default function CoreHeatmap({ data, width = 700 }: CoreHeatmapProps) {
  const latest = data.length > 0 ? data[data.length - 1] : null
  const numCores = latest?.cores.length ?? 0
  const useHeatmap = numCores > 8

  if (!latest || numCores === 0) {
    return <div style={{ padding: 24, color: colors.textMuted, textAlign: 'center' }}>
      Waiting for per-core data...
    </div>
  }

  if (useHeatmap) {
    return <HeatmapGrid cores={latest.cores} />
  }

  return <MultiLineView data={data} width={width} />
}

function HeatmapGrid({ cores }: { cores: { name: string; busy_pct: number }[] }) {
  const cols = Math.ceil(Math.sqrt(cores.length))

  return (
    <div style={{
      display: 'grid',
      gridTemplateColumns: `repeat(${cols}, 1fr)`,
      gap: 4,
    }}>
      {cores.map(c => (
        <div key={c.name} style={{
          background: busyColor(c.busy_pct),
          borderRadius: 4,
          padding: '8px 6px',
          textAlign: 'center',
          transition: 'background 0.3s',
        }}>
          <div style={{ fontSize: 10, color: colors.textMuted }}>{c.name}</div>
          <div style={{ fontSize: 14, fontWeight: 600, color: colors.textPrimary }}>
            {c.busy_pct.toFixed(0)}%
          </div>
        </div>
      ))}
    </div>
  )
}

const LINE_COLORS = [
  '#3b82f6', '#ef4444', '#10b981', '#f59e0b', '#8b5cf6',
  '#ec4899', '#06b6d4', '#f97316',
]

function MultiLineView({ data, width }: { data: CoreDataPoint[]; width: number }) {
  const height = 160
  const padding = { top: 10, right: 20, bottom: 20, left: 40 }
  const innerW = width - padding.left - padding.right
  const innerH = height - padding.top - padding.bottom

  const paths = useMemo(() => {
    if (data.length < 2) return []
    const numCores = data[0].cores.length
    const n = data.length
    const result: { name: string; color: string; d: string }[] = []

    for (let ci = 0; ci < numCores; ci++) {
      const xScale = (i: number) => padding.left + (i / (n - 1)) * innerW
      const yScale = (v: number) => padding.top + innerH - (v / 100) * innerH

      let d = `M ${xScale(0)} ${yScale(data[0].cores[ci]?.busy_pct ?? 0)}`
      for (let i = 1; i < n; i++) {
        const pct = data[i].cores[ci]?.busy_pct ?? 0
        d += ` L ${xScale(i)} ${yScale(pct)}`
      }
      result.push({
        name: data[0].cores[ci]?.name ?? `cpu${ci}`,
        color: LINE_COLORS[ci % LINE_COLORS.length],
        d,
      })
    }
    return result
  }, [data, innerW, innerH, padding.left, padding.top])

  return (
    <div>
      <svg width={width} height={height} style={{ display: 'block' }}>
        <line x1={padding.left} x2={width - padding.right}
              y1={padding.top + innerH} y2={padding.top + innerH}
              stroke={colors.cardBorder} strokeWidth={0.5} />
        {[25, 50, 75, 100].map(v => {
          const y = padding.top + innerH - (v / 100) * innerH
          return <line key={v} x1={padding.left} x2={width - padding.right}
                       y1={y} y2={y} stroke={colors.cardBorder} strokeWidth={0.3} strokeDasharray="3,3" />
        })}
        {paths.map(p => (
          <path key={p.name} d={p.d} fill="none" stroke={p.color} strokeWidth={1.5} opacity={0.8} />
        ))}
      </svg>
      <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', paddingLeft: padding.left }}>
        {paths.slice(0, 8).map(p => (
          <div key={p.name} style={{ display: 'flex', alignItems: 'center', gap: 3 }}>
            <div style={{ width: 12, height: 2, background: p.color, borderRadius: 1 }} />
            <span style={{ fontSize: 10, color: colors.textSecondary }}>{p.name}</span>
          </div>
        ))}
      </div>
    </div>
  )
}
