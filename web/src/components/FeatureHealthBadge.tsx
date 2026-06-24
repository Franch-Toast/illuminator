import { useState, useEffect, useRef } from 'react'
import { useFeatureHealth, FeatureHealthStatus } from '../hooks/useFeatureHealth'
import { api } from '../services/apiClient'
import { colors } from '../styles/theme'

const statusConfig: Record<FeatureHealthStatus, { color: string; icon: string; label: string; hint: string }> = {
  active: { color: colors.success, icon: '●', label: 'Data flowing', hint: 'Data is being received normally.' },
  degraded: { color: colors.warnText, icon: '◐', label: 'No data (>5s)', hint: 'No data received for >5s. Backend may be under heavy load or eBPF probe encountered a transient error.' },
  unavailable: { color: '#ef4444', icon: '○', label: 'Unavailable (>15s)', hint: 'No data for >15s. Check: (1) daemon is running, (2) WebSocket connection, (3) eBPF probe loaded correctly.' },
}

interface Props {
  featureName: string
  showLabel?: boolean
}

export default function FeatureHealthBadge({ featureName, showLabel = false }: Props) {
  const health = useFeatureHealth(featureName)
  const cfg = statusConfig[health]
  const [expanded, setExpanded] = useState(false)
  const [featureDetail, setFeatureDetail] = useState<{
    state?: string; tier?: number; errors?: number; uptime_ms?: number; batches_processed?: number
  } | null>(null)
  const panelRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!expanded) return
    api.features().then(res => {
      const f = res.features.find(feat => feat.name === featureName)
      if (f) setFeatureDetail(f)
    }).catch(() => {})
  }, [expanded, featureName])

  useEffect(() => {
    if (!expanded) return
    const handleClick = (e: MouseEvent) => {
      if (panelRef.current && !panelRef.current.contains(e.target as Node)) {
        setExpanded(false)
      }
    }
    document.addEventListener('mousedown', handleClick)
    return () => document.removeEventListener('mousedown', handleClick)
  }, [expanded])

  return (
    <span ref={panelRef} style={{ position: 'relative', display: 'inline-flex', alignItems: 'center' }}>
      <button
        onClick={() => setExpanded(!expanded)}
        style={{
          display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 11,
          color: cfg.color, background: 'none', border: 'none', cursor: 'pointer', padding: '2px 4px',
          borderRadius: 4,
        }}
        title={`${featureName}: ${cfg.label}`}
      >
        <span>{cfg.icon}</span>
        {showLabel && <span>{cfg.label}</span>}
      </button>

      {expanded && (
        <div style={{
          position: 'absolute', top: '100%', left: 0, marginTop: 4, zIndex: 1000,
          background: '#1a1d23', border: `1px solid ${colors.cardBorder}`, borderRadius: 8,
          padding: 14, minWidth: 260, boxShadow: '0 8px 24px rgba(0,0,0,0.5)',
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
            <span style={{ color: cfg.color, fontSize: 14 }}>{cfg.icon}</span>
            <span style={{ fontSize: 12, fontWeight: 600, color: colors.textPrimary }}>{cfg.label}</span>
          </div>

          <p style={{ margin: '0 0 10px', fontSize: 11, color: colors.textMuted, lineHeight: 1.5 }}>
            {cfg.hint}
          </p>

          <div style={{ fontSize: 11, color: colors.textSecondary, borderTop: `1px solid ${colors.cardBorder}`, paddingTop: 8 }}>
            <div style={{ marginBottom: 4 }}>
              <span style={{ color: colors.textMuted }}>Feature: </span>
              <code style={{ fontSize: 10, color: colors.accent }}>{featureName}</code>
            </div>
            {featureDetail && (
              <>
                <div style={{ marginBottom: 2 }}>
                  <span style={{ color: colors.textMuted }}>State: </span>
                  <span style={{ color: featureDetail.state === 'active' ? colors.success : '#f59e0b' }}>
                    {featureDetail.state}
                  </span>
                </div>
                <div style={{ marginBottom: 2 }}>
                  <span style={{ color: colors.textMuted }}>Batches: </span>
                  {featureDetail.batches_processed?.toLocaleString()}
                </div>
                {(featureDetail.errors ?? 0) > 0 && (
                  <div style={{ color: '#ef4444' }}>
                    Errors: {featureDetail.errors}
                  </div>
                )}
              </>
            )}
          </div>

          {health !== 'active' && (
            <div style={{
              marginTop: 10, padding: '6px 8px', background: 'rgba(96,165,250,0.05)',
              border: '1px solid rgba(96,165,250,0.15)', borderRadius: 4, fontSize: 10, color: colors.accent,
            }}>
              Suggested actions:<br />
              {health === 'degraded' && '• Wait a few seconds — may be transient\n• Check Feature Health Dashboard for errors'}
              {health === 'unavailable' && '• Verify daemon is running (curl /healthz)\n• Check WebSocket connection in browser DevTools\n• Review daemon logs for eBPF errors'}
            </div>
          )}
        </div>
      )}
    </span>
  )
}
