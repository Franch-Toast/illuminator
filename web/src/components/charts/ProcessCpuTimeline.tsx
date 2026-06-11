import { useCallback, useMemo, useRef } from 'react'
import { colors } from '../../styles/theme'

interface TimelinePoint {
  timestamp: number
  cpu_user_pct: number
  cpu_sys_pct: number
}

interface ProcessCpuTimelineProps {
  data: TimelinePoint[]
  selectedTimestamp: number | null
  onTimeSelect: (timestamp: number) => void
  width?: number
  height?: number
}

export default function ProcessCpuTimeline({
  data, selectedTimestamp, onTimeSelect, width = 680, height = 140,
}: ProcessCpuTimelineProps) {
  const svgRef = useRef<SVGSVGElement>(null)
  const padding = { top: 10, right: 20, bottom: 24, left: 46 }
  const innerW = width - padding.left - padding.right
  const innerH = height - padding.top - padding.bottom

  const maxY = useMemo(() => {
    if (data.length === 0) return 100
    const max = Math.max(...data.map(d => d.cpu_user_pct + d.cpu_sys_pct))
    return Math.max(max * 1.1, 10)
  }, [data])

  const { userPath, sysPath } = useMemo(() => {
    if (data.length < 2) return { userPath: '', sysPath: '' }
    const n = data.length
    const xScale = (i: number) => padding.left + (i / (n - 1)) * innerW
    const yScale = (v: number) => padding.top + innerH - (v / maxY) * innerH

    let uPath = `M ${xScale(0)} ${yScale(data[0].cpu_user_pct)}`
    let sPath = `M ${xScale(0)} ${yScale(data[0].cpu_user_pct + data[0].cpu_sys_pct)}`
    for (let i = 1; i < n; i++) {
      uPath += ` L ${xScale(i)} ${yScale(data[i].cpu_user_pct)}`
      sPath += ` L ${xScale(i)} ${yScale(data[i].cpu_user_pct + data[i].cpu_sys_pct)}`
    }
    return { userPath: uPath, sysPath: sPath }
  }, [data, maxY, innerW, innerH, padding.left, padding.top])

  const handleClick = useCallback((e: React.MouseEvent<SVGSVGElement>) => {
    if (data.length < 2 || !svgRef.current) return
    const rect = svgRef.current.getBoundingClientRect()
    const x = e.clientX - rect.left - padding.left
    const ratio = Math.max(0, Math.min(1, x / innerW))
    const idx = Math.round(ratio * (data.length - 1))
    if (idx >= 0 && idx < data.length) {
      onTimeSelect(data[idx].timestamp)
    }
  }, [data, innerW, padding.left, onTimeSelect])

  const selectedIdx = useMemo(() => {
    if (!selectedTimestamp || data.length === 0) return -1
    let closest = 0
    let minDiff = Infinity
    for (let i = 0; i < data.length; i++) {
      const diff = Math.abs(data[i].timestamp - selectedTimestamp)
      if (diff < minDiff) { minDiff = diff; closest = i }
    }
    return closest
  }, [data, selectedTimestamp])

  if (data.length < 2) {
    return (
      <div style={{ height, display: 'flex', alignItems: 'center', justifyContent: 'center', color: colors.textMuted, fontSize: 12 }}>
        Collecting CPU data for this process...
      </div>
    )
  }

  const xScale = (i: number) => padding.left + (i / (data.length - 1)) * innerW
  const yScale = (v: number) => padding.top + innerH - (v / maxY) * innerH

  return (
    <div>
      <svg
        ref={svgRef}
        width={width}
        height={height}
        style={{ display: 'block', cursor: 'crosshair' }}
        onClick={handleClick}
      >
        {/* Grid */}
        {[0, 25, 50, 75, 100].filter(v => v <= maxY).map(v => {
          const y = yScale(v)
          return (
            <g key={v}>
              <line x1={padding.left} x2={width - padding.right} y1={y} y2={y}
                    stroke={colors.cardBorder} strokeWidth={0.5} />
              <text x={padding.left - 6} y={y + 4} textAnchor="end"
                    fill={colors.textMuted} fontSize={10}>{v}%</text>
            </g>
          )
        })}

        {/* System (total = user + sys) line */}
        <path d={sysPath} fill="none" stroke="#ef4444" strokeWidth={1.5} opacity={0.7} />
        {/* User line */}
        <path d={userPath} fill="none" stroke="#3b82f6" strokeWidth={2} />

        {/* Selected time indicator */}
        {selectedIdx >= 0 && (
          <g>
            <line
              x1={xScale(selectedIdx)} x2={xScale(selectedIdx)}
              y1={padding.top} y2={padding.top + innerH}
              stroke={colors.accent} strokeWidth={1} strokeDasharray="3,2"
            />
            <circle
              cx={xScale(selectedIdx)}
              cy={yScale(data[selectedIdx].cpu_user_pct + data[selectedIdx].cpu_sys_pct)}
              r={4} fill={colors.accent} stroke="#fff" strokeWidth={1.5}
            />
          </g>
        )}
      </svg>

      <div style={{ display: 'flex', gap: 12, paddingLeft: padding.left, marginTop: 4 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <div style={{ width: 12, height: 2, background: '#3b82f6', borderRadius: 1 }} />
          <span style={{ fontSize: 10, color: colors.textSecondary }}>User</span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <div style={{ width: 12, height: 2, background: '#ef4444', borderRadius: 1 }} />
          <span style={{ fontSize: 10, color: colors.textSecondary }}>User + System</span>
        </div>
        {selectedTimestamp && (
          <span style={{ fontSize: 10, color: colors.accent, marginLeft: 'auto' }}>
            Selected: {new Date(selectedTimestamp).toLocaleTimeString()}
          </span>
        )}
      </div>
    </div>
  )
}
