import { useState } from 'react'
import { api } from '../../services/apiClient'
import { colors } from '../../styles/theme'

interface ExportResult {
  file: string
  features_exported: number
  batches_exported: number
}

const LOOKBACK_OPTIONS = [
  { value: 10, label: '10 batches (~10s)' },
  { value: 30, label: '30 batches (~30s)' },
  { value: 60, label: '60 batches (~1min)' },
]

export default function ExportControl() {
  const [exporting, setExporting] = useState(false)
  const [result, setResult] = useState<ExportResult | null>(null)
  const [lookback, setLookback] = useState(60)
  const [error, setError] = useState('')

  const handleExport = async () => {
    setExporting(true)
    setError('')
    setResult(null)
    try {
      const resp = await api.exportData({ lookback_batches: lookback })
      setResult(resp)
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Export failed')
    } finally {
      setExporting(false)
    }
  }

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', gap: 6,
      padding: '8px 12px', borderRadius: 6,
      border: `1px solid ${colors.cardBorder}`,
      background: colors.cardBg,
      fontSize: 11,
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, color: colors.textSecondary }}>
        <span style={{ fontSize: 13 }}>Export</span>
      </div>

      <select
        value={lookback}
        onChange={(e) => setLookback(Number(e.target.value))}
        style={{
          background: colors.bg, color: colors.textPrimary,
          border: `1px solid ${colors.cardBorder}`,
          borderRadius: 4, padding: '3px 6px', fontSize: 11,
        }}
      >
        {LOOKBACK_OPTIONS.map(o => (
          <option key={o.value} value={o.value}>{o.label}</option>
        ))}
      </select>

      <button
        onClick={handleExport}
        disabled={exporting}
        style={{
          display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 4,
          padding: '5px 8px', borderRadius: 4,
          border: `1px solid ${colors.success}`,
          background: exporting ? colors.cardBg : 'transparent',
          color: colors.success,
          cursor: exporting ? 'wait' : 'pointer', fontSize: 11,
          opacity: exporting ? 0.6 : 1,
        }}
      >
        {exporting ? 'Exporting...' : 'Export .ilr'}
      </button>

      {result && (
        <div style={{ color: colors.success, fontSize: 10, lineHeight: 1.4 }}>
          {result.features_exported} features, {result.batches_exported} batches
        </div>
      )}
      {error && (
        <div style={{ color: colors.danger, fontSize: 10 }}>{error}</div>
      )}
    </div>
  )
}
