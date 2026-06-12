import { useEffect, useRef } from 'react'
import { useTimeStore } from '../../stores/useTimeStore'
import { colors } from '../../styles/theme'
import { AddAnnotationButton } from '../AnnotationOverlay'

const WINDOW_OPTIONS = [
  { label: '30s', ms: 30_000 },
  { label: '1m', ms: 60_000 },
  { label: '5m', ms: 5 * 60_000 },
  { label: '15m', ms: 15 * 60_000 },
]

export default function TimeControls() {
  const { mode, range, windowMs, togglePause, setWindowMs, tick } = useTimeStore()
  const tickRef = useRef<ReturnType<typeof setInterval>>()

  useEffect(() => {
    tickRef.current = setInterval(tick, 1000)
    return () => clearInterval(tickRef.current)
  }, [tick])

  const fmtTime = (ts: number) =>
    new Date(ts).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' })

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 12,
      padding: '6px 16px',
      background: colors.cardBg,
      borderBottom: `1px solid ${colors.cardBorder}`,
      fontSize: 13,
    }}>
      <button
        onClick={togglePause}
        title="Space to toggle (pause freezes data updates)"
        style={{
          display: 'flex', alignItems: 'center', gap: 5,
          padding: '4px 14px', borderRadius: 6,
          border: `1px solid ${mode === 'live' ? '#4ade80' : '#f59e0b'}`,
          background: mode === 'live' ? 'rgba(74,222,128,0.1)' : 'rgba(245,158,11,0.15)',
          color: mode === 'live' ? '#4ade80' : '#f59e0b',
          cursor: 'pointer', fontSize: 12, fontWeight: 600,
          transition: 'all 0.2s ease',
        }}
      >
        {mode === 'live' ? (
          <span style={{ width: 8, height: 8, borderRadius: '50%', background: '#4ade80',
            animation: 'pulse 2s infinite',
          }} />
        ) : (
          <span style={{ fontSize: 11 }}>⏸</span>
        )}
        {mode === 'live' ? 'LIVE' : 'PAUSED'}
      </button>

      <div style={{ display: 'flex', gap: 4 }}>
        {WINDOW_OPTIONS.map(opt => (
          <button
            key={opt.ms}
            onClick={() => setWindowMs(opt.ms)}
            style={{
              padding: '3px 10px', borderRadius: 4, fontSize: 11,
              border: `1px solid ${windowMs === opt.ms ? colors.accent : colors.cardBorder}`,
              background: windowMs === opt.ms ? colors.activeBg : 'transparent',
              color: windowMs === opt.ms ? colors.accent : colors.textSecondary,
              cursor: 'pointer',
            }}
          >
            {opt.label}
          </button>
        ))}
      </div>

      <div style={{ color: colors.textMuted, fontSize: 11, fontFamily: 'monospace' }}>
        {fmtTime(range.start)} — {fmtTime(range.end)}
      </div>

      <div style={{ marginLeft: 'auto' }}>
        <AddAnnotationButton />
      </div>
    </div>
  )
}
