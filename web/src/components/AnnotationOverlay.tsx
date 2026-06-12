import { useState, useCallback } from 'react'
import { colors } from '../styles/theme'
import { useAnnotationStore, getAnnotationColor, getAnnotationIcon } from '../stores/useAnnotationStore'
import type { Annotation, AnnotationType } from '../stores/useAnnotationStore'
import { useTimeStore } from '../stores/useTimeStore'

interface AnnotationOverlayProps {
  width: number
  height?: number
}

export default function AnnotationOverlay({ width, height = 24 }: AnnotationOverlayProps) {
  const { range } = useTimeStore()
  const annotations = useAnnotationStore(s => s.getInRange(range.start, range.end))
  const [tooltip, setTooltip] = useState<{ ann: Annotation; x: number } | null>(null)

  if (annotations.length === 0) return null

  const rangeMs = range.end - range.start

  return (
    <div style={{ position: 'relative', width, height, marginBottom: 4 }}>
      {annotations.map(ann => {
        const x = ((ann.timestamp - range.start) / rangeMs) * width
        if (x < 0 || x > width) return null
        const color = ann.color ?? getAnnotationColor(ann.type)
        return (
          <div
            key={ann.id}
            onMouseEnter={() => setTooltip({ ann, x })}
            onMouseLeave={() => setTooltip(null)}
            style={{
              position: 'absolute',
              left: x - 6,
              top: 0,
              width: 12,
              height,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              cursor: 'pointer',
              fontSize: 10,
            }}
          >
            <div style={{
              width: 2,
              height: '100%',
              background: color,
              opacity: 0.7,
              position: 'absolute',
              left: 5,
            }} />
            <span style={{ position: 'relative', zIndex: 1, fontSize: 11 }}>
              {getAnnotationIcon(ann.type)}
            </span>
          </div>
        )
      })}

      {tooltip && (
        <div style={{
          position: 'absolute',
          left: Math.min(tooltip.x, width - 180),
          top: height + 4,
          background: '#1a1d23',
          border: `1px solid ${colors.cardBorder}`,
          borderRadius: 6,
          padding: '6px 10px',
          fontSize: 11,
          color: colors.textPrimary,
          zIndex: 100,
          minWidth: 140,
          boxShadow: '0 4px 12px rgba(0,0,0,0.4)',
        }}>
          <div style={{ fontWeight: 600, marginBottom: 2 }}>{tooltip.ann.label}</div>
          <div style={{ color: colors.textMuted, fontSize: 10 }}>
            {new Date(tooltip.ann.timestamp).toLocaleTimeString()}
          </div>
          {tooltip.ann.description && (
            <div style={{ color: colors.textSecondary, marginTop: 4, fontSize: 10 }}>
              {tooltip.ann.description}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

export function AddAnnotationButton() {
  const [open, setOpen] = useState(false)
  const [label, setLabel] = useState('')
  const [type, setType] = useState<AnnotationType>('manual')
  const add = useAnnotationStore(s => s.add)

  const handleAdd = useCallback(() => {
    if (!label.trim()) return
    add({ timestamp: Date.now(), type, label: label.trim() })
    setLabel('')
    setOpen(false)
  }, [label, type, add])

  if (!open) {
    return (
      <button
        onClick={() => setOpen(true)}
        style={{
          padding: '3px 8px', borderRadius: 4, fontSize: 10,
          border: `1px solid ${colors.cardBorder}`,
          background: 'transparent', color: colors.textMuted,
          cursor: 'pointer',
        }}
        title="Add annotation marker"
      >
        + Mark
      </button>
    )
  }

  return (
    <div style={{
      display: 'flex', gap: 6, alignItems: 'center',
      background: colors.cardBg, padding: '4px 8px',
      borderRadius: 6, border: `1px solid ${colors.cardBorder}`,
    }}>
      <select
        value={type}
        onChange={e => setType(e.target.value as AnnotationType)}
        style={{
          background: '#1a1d23', border: `1px solid ${colors.cardBorder}`,
          color: colors.textPrimary, fontSize: 10, borderRadius: 4, padding: '2px 4px',
        }}
      >
        <option value="manual">Manual</option>
        <option value="deploy">Deploy</option>
        <option value="alert">Alert</option>
        <option value="spike">Spike</option>
        <option value="gc">GC</option>
      </select>
      <input
        value={label}
        onChange={e => setLabel(e.target.value)}
        onKeyDown={e => e.key === 'Enter' && handleAdd()}
        placeholder="Label..."
        style={{
          background: '#1a1d23', border: `1px solid ${colors.cardBorder}`,
          color: colors.textPrimary, fontSize: 11, borderRadius: 4,
          padding: '3px 6px', width: 100,
        }}
        autoFocus
      />
      <button
        onClick={handleAdd}
        style={{
          padding: '2px 6px', borderRadius: 4, fontSize: 10,
          border: 'none', background: colors.accent,
          color: '#fff', cursor: 'pointer',
        }}
      >
        Add
      </button>
      <button
        onClick={() => setOpen(false)}
        style={{
          padding: '2px 6px', borderRadius: 4, fontSize: 10,
          border: `1px solid ${colors.cardBorder}`,
          background: 'transparent', color: colors.textMuted, cursor: 'pointer',
        }}
      >
        Cancel
      </button>
    </div>
  )
}
