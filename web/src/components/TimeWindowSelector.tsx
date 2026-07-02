/**
 * TimeWindowSelector — RFC v3 5.1 时间窗口选择器
 *
 * 允许用户选择前端数据缓冲区大小，并导出当前窗口内的数据为 .ilr 文件。
 */

import { useState } from 'react'
import { dataBus } from '../services/dataBus'
import { colors } from '../styles/theme'

const WINDOW_OPTIONS = [
  { label: '30 秒', value: 30 },
  { label: '1 分钟', value: 60 },
  { label: '2 分钟', value: 120 },
  { label: '5 分钟', value: 300 },
]

function saveFrontendData(windowSeconds: number) {
  const features = dataBus.getAvailableFeatures()
  const lines: string[] = []

  lines.push(JSON.stringify({
    format: 'ilr',
    version: 2,
    generated_at: Date.now(),
    generated_by: 'illuminator-web',
    features,
    window_seconds: windowSeconds,
  }))

  for (const feature of features) {
    const recent = dataBus.getRecent(feature, windowSeconds * 2)
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
}

export default function TimeWindowSelector() {
  const [windowSec, setWindowSec] = useState(() => dataBus.getWindowSize())

  const handleChange = (val: number) => {
    setWindowSec(val)
    dataBus.setWindowSize(val)
  }

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 8,
      padding: '6px 12px', background: colors.cardBg,
      borderRadius: 6, border: `1px solid ${colors.cardBorder}`,
    }}>
      <label style={{ fontSize: 11, color: colors.textMuted, whiteSpace: 'nowrap' }}>
        Buffer:
      </label>
      <select
        value={windowSec}
        onChange={(e) => handleChange(Number(e.target.value))}
        style={{
          background: '#1a1d23', color: colors.textPrimary,
          border: `1px solid ${colors.cardBorder}`, borderRadius: 4,
          padding: '3px 6px', fontSize: 11, cursor: 'pointer',
        }}
      >
        {WINDOW_OPTIONS.map(opt => (
          <option key={opt.value} value={opt.value}>{opt.label}</option>
        ))}
      </select>
      <button
        onClick={() => saveFrontendData(windowSec)}
        style={{
          padding: '3px 10px', borderRadius: 4, fontSize: 11,
          background: 'rgba(37, 99, 235, 0.15)', color: '#60a5fa',
          border: '1px solid rgba(37, 99, 235, 0.3)', cursor: 'pointer',
          whiteSpace: 'nowrap',
        }}
      >
        Export .ilr
      </button>
    </div>
  )
}
