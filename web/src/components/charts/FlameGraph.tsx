import { useState, useMemo, useCallback, useRef, useEffect } from 'react'
import { colors } from '../../styles/theme'
import type { FlameNode } from '../../workers/flameGraphWorker'
import { useFlameZoom, useFlameSearch } from '../../hooks/useFlameGraph'

interface FlameGraphProps {
  root: FlameNode | null
  isDiff?: boolean
  height?: number
  onFunctionClick?: (name: string) => void
}

const FRAME_HEIGHT = 18
const MIN_WIDTH_PX = 1

function useContainerWidth(ref: React.RefObject<HTMLDivElement | null>): number {
  const [width, setWidth] = useState(800)

  useEffect(() => {
    const el = ref.current
    if (!el) return

    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width
        if (w > 0) setWidth(Math.floor(w))
      }
    })
    observer.observe(el)
    setWidth(Math.floor(el.clientWidth || 800))
    return () => observer.disconnect()
  }, [ref])

  return width
}

function getColor(node: FlameNode & { diff?: number }, isDiff: boolean, searchMatches: Set<string>): string {
  if (searchMatches.size > 0 && !searchMatches.has(node.name)) {
    return 'rgba(100,100,100,0.3)'
  }
  if (isDiff && node.diff !== undefined) {
    if (node.diff > 2) return '#dc2626'
    if (node.diff > 0.5) return '#f97316'
    if (node.diff < -2) return '#2563eb'
    if (node.diff < -0.5) return '#60a5fa'
    return '#4b5563'
  }
  const hue = (node.name.split('').reduce((a, c) => a + c.charCodeAt(0), 0) % 40) + 10
  return `hsl(${hue}, 70%, 55%)`
}

export default function FlameGraph({ root, isDiff = false, height = 400, onFunctionClick }: FlameGraphProps) {
  const { zoomNode, breadcrumbs, zoomIn, zoomOut, reset } = useFlameZoom(root)
  const { query, search, matches, totalPct } = useFlameSearch(root)
  const [tooltip, setTooltip] = useState<{ x: number; y: number; node: FlameNode } | null>(null)
  const containerRef = useRef<HTMLDivElement>(null)
  const width = useContainerWidth(containerRef)

  const searchSet = useMemo(() => new Set(matches), [matches])

  const renderNode = useCallback((node: FlameNode & { diff?: number }, x: number, w: number, depth: number): JSX.Element[] => {
    if (w < MIN_WIDTH_PX) return []

    const elements: JSX.Element[] = []
    const y = depth * FRAME_HEIGHT
    const color = getColor(node, isDiff, searchSet)

    elements.push(
      <rect
        key={`${node.name}-${depth}-${x}`}
        x={x} y={y} width={Math.max(w - 0.5, 0.5)} height={FRAME_HEIGHT - 1}
        fill={color} rx={1}
        style={{ cursor: 'pointer' }}
        onMouseEnter={(e) => setTooltip({ x: e.clientX, y: e.clientY, node })}
        onMouseLeave={() => setTooltip(null)}
        onClick={() => {
          if (node.children.length > 0) zoomIn(node)
          onFunctionClick?.(node.name)
        }}
      />
    )

    if (w > 40) {
      const label = node.name.length > w / 6 ? node.name.slice(0, Math.floor(w / 6)) + '...' : node.name
      elements.push(
        <text
          key={`t-${node.name}-${depth}-${x}`}
          x={x + 3} y={y + 12}
          fontSize={10} fill="#fff"
          style={{ pointerEvents: 'none' }}
        >
          {label}
        </text>
      )
    }

    let childX = x
    const total = node.value || 1
    for (const child of node.children) {
      const childW = (child.value / total) * w
      elements.push(...renderNode(child, childX, childW, depth + 1))
      childX += childW
    }

    return elements
  }, [isDiff, searchSet, zoomIn, onFunctionClick])

  const frames = useMemo(() => {
    if (!zoomNode) return []
    return renderNode(zoomNode, 0, width, 0)
  }, [zoomNode, width, renderNode])

  if (!root) {
    return (
      <div style={{ padding: 24, textAlign: 'center', color: colors.textMuted }}>
        No profile data available
      </div>
    )
  }

  return (
    <div ref={containerRef} style={{ position: 'relative' }}>
      {/* Toolbar */}
      <div style={{ display: 'flex', gap: 8, marginBottom: 8, alignItems: 'center' }}>
        <input
          type="text"
          placeholder="Search functions..."
          value={query}
          onChange={(e) => search(e.target.value)}
          style={{
            flex: 1, padding: '4px 8px', fontSize: 12, borderRadius: 4,
            background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
            color: colors.textPrimary, outline: 'none',
          }}
        />
        {query && (
          <span style={{ fontSize: 11, color: colors.textMuted }}>
            {totalPct.toFixed(1)}% matched
          </span>
        )}
        {breadcrumbs.length > 0 && (
          <button onClick={reset} style={{
            padding: '3px 8px', borderRadius: 4, fontSize: 11,
            border: `1px solid ${colors.cardBorder}`, background: colors.cardBg,
            color: colors.textSecondary, cursor: 'pointer',
          }}>
            Reset Zoom
          </button>
        )}
      </div>

      {/* Breadcrumbs */}
      {breadcrumbs.length > 0 && (
        <div style={{ display: 'flex', gap: 4, marginBottom: 6, flexWrap: 'wrap' }}>
          <span onClick={reset} style={{ fontSize: 11, color: colors.accent, cursor: 'pointer' }}>root</span>
          {breadcrumbs.map((bc, i) => (
            <span key={i} style={{ fontSize: 11, color: colors.textMuted }}>
              {' > '}
              <span
                onClick={() => zoomIn === undefined ? undefined : undefined}
                style={{ color: i === breadcrumbs.length - 1 ? colors.textPrimary : colors.accent, cursor: 'pointer' }}
              >
                {bc.name.length > 30 ? bc.name.slice(0, 30) + '...' : bc.name}
              </span>
            </span>
          ))}
          <button onClick={zoomOut} style={{
            marginLeft: 8, padding: '1px 6px', borderRadius: 3, fontSize: 10,
            border: `1px solid ${colors.cardBorder}`, background: 'transparent',
            color: colors.textSecondary, cursor: 'pointer',
          }}>
            Back
          </button>
        </div>
      )}

      {/* Flame SVG */}
      <svg
        width="100%"
        height={height}
        viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="xMinYMin meet"
        style={{ display: 'block', background: colors.bg, overflow: 'hidden' }}
      >
        {frames}
      </svg>

      {/* Tooltip */}
      {tooltip && (
        <div style={{
          position: 'fixed', left: tooltip.x + 12, top: tooltip.y - 40,
          background: '#1a1d23', border: `1px solid ${colors.cardBorder}`,
          borderRadius: 6, padding: '6px 10px', fontSize: 11,
          color: colors.textPrimary, pointerEvents: 'none', zIndex: 9999,
          maxWidth: 400, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
        }}>
          <div style={{ fontWeight: 600 }}>{tooltip.node.name}</div>
          <div style={{ color: colors.textMuted }}>
            {tooltip.node.value} samples ({((tooltip.node.value / (root?.value || 1)) * 100).toFixed(1)}%)
            {isDiff && (tooltip.node as FlameNode & { diff?: number }).diff !== undefined && (
              <span style={{ marginLeft: 8, color: (tooltip.node as FlameNode & { diff?: number }).diff! > 0 ? colors.danger : colors.accent }}>
                {(tooltip.node as FlameNode & { diff?: number }).diff! > 0 ? '+' : ''}{(tooltip.node as FlameNode & { diff?: number }).diff!.toFixed(1)}%
              </span>
            )}
          </div>
        </div>
      )}

      {/* Diff legend */}
      {isDiff && (
        <div style={{ display: 'flex', gap: 12, marginTop: 8, fontSize: 11, color: colors.textMuted }}>
          <span><span style={{ color: '#dc2626' }}>■</span> Regression (&gt;+2%)</span>
          <span><span style={{ color: '#f97316' }}>■</span> Slight increase</span>
          <span><span style={{ color: '#4b5563' }}>■</span> No change</span>
          <span><span style={{ color: '#60a5fa' }}>■</span> Slight decrease</span>
          <span><span style={{ color: '#2563eb' }}>■</span> Improvement (&gt;-2%)</span>
        </div>
      )}
    </div>
  )
}
