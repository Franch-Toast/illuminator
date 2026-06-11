import { useState } from 'react'
import { useFeatureList } from '../hooks/useFeatureStream'
import { api, FeatureEntry } from '../services/apiClient'
import { colors } from '../styles/theme'

function formatUptime(ms: number): string {
  if (ms === 0) return '-'
  const sec = Math.floor(ms / 1000)
  if (sec < 60) return `${sec}s`
  const min = Math.floor(sec / 60)
  if (min < 60) return `${min}m ${sec % 60}s`
  const hr = Math.floor(min / 60)
  return `${hr}h ${min % 60}m`
}

function stateColor(state: string): string {
  switch (state) {
    case 'active': return colors.success
    case 'paused': return colors.amber
    case 'starting':
    case 'stopping': return colors.accent
    default: return '#6b7280'
  }
}

function FeatureRow({ feature, onRefresh }: { feature: FeatureEntry; onRefresh: () => void }) {
  const [recording, setRecording] = useState(feature.is_recording)

  const handleStart = async () => {
    try { await api.featureStart(feature.name); onRefresh() } catch { /* */ }
  }
  const handleStop = async () => {
    try { await api.featureStop(feature.name); setRecording(false); onRefresh() } catch { /* */ }
  }
  const handlePause = async () => {
    try { await api.featurePause(feature.name); onRefresh() } catch { /* */ }
  }
  const handleResume = async () => {
    try { await api.featureResume(feature.name); onRefresh() } catch { /* */ }
  }
  const handleRecordToggle = async () => {
    try {
      if (recording) {
        await api.featureRecordStop(feature.name)
        setRecording(false)
      } else {
        await api.featureRecordStart(feature.name)
        setRecording(true)
      }
    } catch { /* */ }
  }

  const btnStyle: React.CSSProperties = {
    padding: '4px 10px', borderRadius: 4, border: 'none',
    cursor: 'pointer', fontSize: 12, fontWeight: 500,
  }

  return (
    <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
      <td style={{ padding: '10px 12px' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <div style={{
            width: 8, height: 8, borderRadius: '50%',
            background: stateColor(feature.state),
            boxShadow: feature.state === 'active' ? `0 0 4px ${colors.success}` : 'none',
          }} />
          <span style={{ fontWeight: 500, color: colors.textPrimary }}>
            {feature.display_name || feature.name}
          </span>
        </div>
      </td>
      <td style={{ padding: '10px 12px', color: colors.textSecondary, fontSize: 12, textTransform: 'capitalize' }}>
        {feature.state}
      </td>
      <td style={{ padding: '10px 12px', color: colors.textMuted, fontSize: 12 }}>
        {feature.category || '-'}
      </td>
      <td style={{ padding: '10px 12px', color: colors.textSecondary, fontSize: 12 }}>
        {formatUptime(feature.uptime_ms)}
      </td>
      <td style={{ padding: '10px 12px', color: colors.textSecondary, fontSize: 12 }}>
        {feature.records_processed > 0 ? feature.records_processed.toLocaleString() : '-'}
      </td>
      <td style={{ padding: '10px 12px' }}>
        <div style={{ display: 'flex', gap: 6 }}>
          {feature.state === 'inactive' && (
            <button style={{ ...btnStyle, background: colors.success, color: '#000' }} onClick={handleStart}>
              Start
            </button>
          )}
          {feature.state === 'active' && (
            <>
              <button
                style={{
                  ...btnStyle,
                  background: recording ? '#ef4444' : '#6b7280',
                  color: '#fff',
                }}
                onClick={handleRecordToggle}
              >
                {recording ? '⏹ Stop Rec' : '⏺ Record'}
              </button>
              <button style={{ ...btnStyle, background: colors.amber, color: '#000' }} onClick={handlePause}>
                Pause
              </button>
              <button style={{ ...btnStyle, background: '#ef4444', color: '#fff' }} onClick={handleStop}>
                Stop
              </button>
            </>
          )}
          {feature.state === 'paused' && (
            <>
              <button style={{ ...btnStyle, background: colors.accent, color: '#000' }} onClick={handleResume}>
                Resume
              </button>
              <button style={{ ...btnStyle, background: '#ef4444', color: '#fff' }} onClick={handleStop}>
                Stop
              </button>
            </>
          )}
        </div>
      </td>
    </tr>
  )
}

export default function Features() {
  const { features, loading, refresh } = useFeatureList()

  const activeCount = features.filter(f => f.state === 'active').length
  const totalCount = features.length

  const thStyle: React.CSSProperties = {
    padding: '8px 12px', textAlign: 'left', fontSize: 11,
    color: colors.textMuted, fontWeight: 600, textTransform: 'uppercase',
    borderBottom: `1px solid ${colors.cardBorder}`,
  }

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', alignItems: 'center', marginBottom: 20 }}>
        <h2 style={{ margin: 0, fontSize: 18, color: colors.textPrimary }}>
          Feature Manager
        </h2>
        <span style={{ marginLeft: 12, fontSize: 13, color: colors.textSecondary }}>
          {activeCount}/{totalCount} active
        </span>
      </div>

      {loading && features.length === 0 ? (
        <div style={{ color: colors.textMuted, padding: 40, textAlign: 'center' }}>
          Loading features...
        </div>
      ) : features.length === 0 ? (
        <div style={{ color: colors.textMuted, padding: 40, textAlign: 'center' }}>
          No features registered. Check your illuminator.yaml configuration.
        </div>
      ) : (
        <div style={{
          background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
          borderRadius: 8, overflow: 'hidden',
        }}>
          <table style={{ width: '100%', borderCollapse: 'collapse' }}>
            <thead>
              <tr>
                <th style={thStyle}>Feature</th>
                <th style={thStyle}>State</th>
                <th style={thStyle}>Category</th>
                <th style={thStyle}>Uptime</th>
                <th style={thStyle}>Records</th>
                <th style={thStyle}>Actions</th>
              </tr>
            </thead>
            <tbody>
              {features.map(f => (
                <FeatureRow key={f.name} feature={f} onRefresh={refresh} />
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
