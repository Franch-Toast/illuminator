import React, { useState, useRef, useEffect } from 'react'
import { colors } from '../styles/theme'
import {
  exportTimeSeriesCSV,
  exportTimeSeriesJSON,
  exportEChartAsPNG,
  exportFoldedFormat,
  type ExportFormat,
} from '../services/exportService'

interface ExportOption {
  id: string
  label: string
  format: ExportFormat
  action: () => void
}

interface ExportMenuProps {
  options: ExportOption[]
  label?: string
}

export default function ExportMenu({ options, label = 'Export' }: ExportMenuProps) {
  const [open, setOpen] = useState(false)
  const ref = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) {
        setOpen(false)
      }
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  return (
    <div ref={ref} style={{ position: 'relative', display: 'inline-block' }}>
      <button
        onClick={() => setOpen(v => !v)}
        style={{
          padding: '5px 12px', borderRadius: 4, fontSize: 12,
          border: `1px solid ${colors.cardBorder}`, background: colors.cardBg,
          color: colors.textSecondary, cursor: 'pointer',
          display: 'flex', alignItems: 'center', gap: 4,
        }}
      >
        <span style={{ fontSize: 11 }}>⬇</span> {label}
      </button>

      {open && (
        <div style={{
          position: 'absolute', top: '100%', right: 0, marginTop: 4, zIndex: 100,
          background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
          borderRadius: 6, boxShadow: '0 4px 12px rgba(0,0,0,0.4)', minWidth: 160,
          overflow: 'hidden',
        }}>
          {options.map(opt => (
            <button
              key={opt.id}
              onClick={() => { opt.action(); setOpen(false) }}
              style={{
                display: 'block', width: '100%', textAlign: 'left',
                padding: '8px 14px', border: 'none', background: 'transparent',
                color: colors.textSecondary, fontSize: 12, cursor: 'pointer',
              }}
              onMouseEnter={e => { (e.target as HTMLElement).style.background = colors.activeBg }}
              onMouseLeave={e => { (e.target as HTMLElement).style.background = 'transparent' }}
            >
              <span style={{ marginRight: 8, opacity: 0.6 }}>
                {opt.format === 'csv' ? '📊' : opt.format === 'json' ? '{ }' : opt.format === 'folded' ? '🔥' : opt.format === 'png' ? '🖼' : '📄'}
              </span>
              {opt.label}
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

export function useChartExportOptions(chartRef: React.RefObject<HTMLDivElement | null>, data: Array<Record<string, unknown>>, prefix: string) {
  const options: ExportOption[] = [
    {
      id: 'csv',
      label: 'Time Series (CSV)',
      format: 'csv',
      action: () => exportTimeSeriesCSV(data as Array<{ timestamp: number; [k: string]: number | string }>, prefix),
    },
    {
      id: 'json',
      label: 'Time Series (JSON)',
      format: 'json',
      action: () => exportTimeSeriesJSON(data as Array<{ timestamp: number; [k: string]: number | string }>, prefix),
    },
    {
      id: 'png',
      label: 'Chart Image (PNG)',
      format: 'png',
      action: () => exportEChartAsPNG(chartRef.current, prefix),
    },
  ]
  return options
}

export function useFlameGraphExportOptions(root: unknown, prefix: string) {
  const options: ExportOption[] = [
    {
      id: 'folded',
      label: 'Folded Stacks (.folded)',
      format: 'folded',
      action: () => exportFoldedFormat(root as Parameters<typeof exportFoldedFormat>[0], prefix),
    },
    {
      id: 'json',
      label: 'Flame Tree (JSON)',
      format: 'json',
      action: () => {
        if (!root) return
        const blob = new Blob([JSON.stringify(root, null, 2)], { type: 'application/json' })
        const url = URL.createObjectURL(blob)
        const a = document.createElement('a')
        a.href = url
        a.download = `${prefix}.json`
        a.click()
        URL.revokeObjectURL(url)
      },
    },
  ]
  return options
}
