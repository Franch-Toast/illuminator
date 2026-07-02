import { useState } from 'react'
import { dataBus } from '../../services/dataBus'
import { colors } from '../../styles/theme'

const LOOKBACK_OPTIONS = [
  { value: 30, label: '~30s' },
  { value: 60, label: '~1min' },
  { value: 120, label: '~2min' },
]

export default function ExportControl() {
  const [lookback, setLookback] = useState(60)
  const [exported, setExported] = useState(false)

  const handleExport = () => {
    const features = dataBus.getAvailableFeatures()
    if (features.length === 0) {
      return
    }

    const lines: string[] = []
    lines.push(JSON.stringify({
      format: 'ilr',
      version: 2,
      generated_at: Date.now(),
      generated_by: 'illuminator-web',
      features,
      window_seconds: lookback,
    }))

    for (const feature of features) {
      const recent = dataBus.getRecent(feature, lookback * 2)
      for (const msg of recent) {
        lines.push(JSON.stringify(msg))
      }
    }

    const blob = new Blob([lines.join('\n') + '\n'], {
      type: 'application/x-illuminator-recording'
    })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `illuminator_${new Date().toISOString().replace(/[:.]/g, '-')}.ilr`
    a.click()
    URL.revokeObjectURL(url)

    setExported(true)
    setTimeout(() => setExported(false), 2000)
  }

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 6,
      fontSize: 11, color: colors.textSecondary,
    }}>
      <select
        value={lookback}
        onChange={(e) => setLookback(Number(e.target.value))}
        style={{
          background: '#1a1d23', color: colors.textPrimary,
          border: `1px solid ${colors.cardBorder}`,
          borderRadius: 4, padding: '2px 4px', fontSize: 10,
        }}
      >
        {LOOKBACK_OPTIONS.map(o => (
          <option key={o.value} value={o.value}>{o.label}</option>
        ))}
      </select>

      <button
        onClick={handleExport}
        style={{
          padding: '3px 8px', borderRadius: 4, fontSize: 10,
          border: `1px solid ${exported ? colors.success : colors.cardBorder}`,
          background: 'transparent',
          color: exported ? colors.success : colors.textMuted,
          cursor: 'pointer',
        }}
      >
        {exported ? 'Saved!' : 'Export'}
      </button>
    </div>
  )
}
