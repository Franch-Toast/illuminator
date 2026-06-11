import { useEffect, useState } from 'react'
import { useFeatureList } from '../hooks/useFeatureStream'
import { api } from '../services/apiClient'
import { colors } from '../styles/theme'

const card: React.CSSProperties = {
  background: colors.cardBg, borderRadius: 8, padding: 20,
  border: `1px solid ${colors.cardBorder}`,
}

function stateColor(state: string): string {
  switch (state) {
    case 'active': return colors.success
    case 'paused': return colors.amber
    default: return '#6b7280'
  }
}

export default function OverviewPage() {
  const { features } = useFeatureList()
  const [version, setVersion] = useState('...')
  const [uptime, setUptime] = useState('')

  useEffect(() => {
    api.healthz()
      .then(d => setVersion(d.version ?? '?'))
      .catch(() => {})
  }, [])

  useEffect(() => {
    const start = Date.now()
    const timer = setInterval(() => {
      const sec = Math.floor((Date.now() - start) / 1000)
      if (sec < 60) setUptime(`${sec}s`)
      else if (sec < 3600) setUptime(`${Math.floor(sec / 60)}m ${sec % 60}s`)
      else setUptime(`${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`)
    }, 1000)
    return () => clearInterval(timer)
  }, [])

  const activeCount = features.filter(f => f.state === 'active').length
  const categories = [...new Set(features.map(f => f.category || 'default'))]

  return (
    <div style={{ padding: 24, maxWidth: 1200 }}>
      <h2 style={{ margin: '0 0 20px', fontSize: 20, color: colors.textPrimary }}>
        System Overview
      </h2>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: 16, marginBottom: 24 }}>
        <div style={card}>
          <div style={{ fontSize: 11, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 6 }}>Version</div>
          <div style={{ fontSize: 18, fontWeight: 600, color: colors.accent, fontFamily: 'monospace' }}>{version}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 6 }}>Session Uptime</div>
          <div style={{ fontSize: 18, fontWeight: 600, color: colors.textPrimary }}>{uptime || '-'}</div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 6 }}>Features</div>
          <div style={{ fontSize: 18, fontWeight: 600, color: colors.textPrimary }}>
            <span style={{ color: colors.success }}>{activeCount}</span>
            <span style={{ color: colors.textMuted, fontSize: 14 }}> / {features.length}</span>
          </div>
        </div>
        <div style={card}>
          <div style={{ fontSize: 11, color: colors.textMuted, textTransform: 'uppercase', marginBottom: 6 }}>Categories</div>
          <div style={{ fontSize: 18, fontWeight: 600, color: colors.textPrimary }}>{categories.length}</div>
        </div>
      </div>

      <div style={card}>
        <h3 style={{ margin: '0 0 16px', fontSize: 15, color: colors.textPrimary }}>Registered Features</h3>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
          <thead>
            <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              <th style={{ padding: '8px 12px', textAlign: 'left', color: colors.textMuted, fontSize: 11, textTransform: 'uppercase' }}>Feature</th>
              <th style={{ padding: '8px 12px', textAlign: 'left', color: colors.textMuted, fontSize: 11, textTransform: 'uppercase' }}>Category</th>
              <th style={{ padding: '8px 12px', textAlign: 'left', color: colors.textMuted, fontSize: 11, textTransform: 'uppercase' }}>State</th>
            </tr>
          </thead>
          <tbody>
            {features.map(f => (
              <tr key={f.name} style={{ borderBottom: `1px solid ${colors.cardBorder}22` }}>
                <td style={{ padding: '10px 12px', color: colors.textPrimary, fontWeight: 500 }}>
                  {f.display_name || f.name}
                </td>
                <td style={{ padding: '10px 12px', color: colors.textSecondary }}>
                  {f.category || 'default'}
                </td>
                <td style={{ padding: '10px 12px' }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <div style={{
                      width: 8, height: 8, borderRadius: '50%',
                      background: stateColor(f.state),
                      boxShadow: f.state === 'active' ? `0 0 4px ${colors.success}` : 'none',
                    }} />
                    <span style={{ color: colors.textSecondary, textTransform: 'capitalize' }}>{f.state}</span>
                  </div>
                </td>
              </tr>
            ))}
            {features.length === 0 && (
              <tr>
                <td colSpan={3} style={{ padding: 24, textAlign: 'center', color: colors.textMuted }}>
                  No features registered
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}
