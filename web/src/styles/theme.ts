import type { CSSProperties } from 'react'

export const colors = {
  bg: '#0f1117',
  cardBg: '#1a1d23',
  cardBorder: '#2a2d35',
  activeBg: '#252830',
  textPrimary: '#e0e0e0',
  textSecondary: '#b0b0b0',
  textMuted: '#888',
  textDimmed: '#666',
  accent: '#60a5fa',
  danger: '#f87171',
  dangerBg: '#331a1a',
  dangerBorder: '#7f1d1d',
  warnBg: '#332b00',
  warnBorder: '#665500',
  warnText: '#fbbf24',
  success: '#4ade80',
  blue: '#3b82f6',
  red: '#ef4444',
  amber: '#f59e0b',
  purple: '#a855f7',
  gray: '#6b7280',
} as const

export const card: CSSProperties = {
  background: colors.cardBg,
  borderRadius: 8,
  padding: 20,
  border: `1px solid ${colors.cardBorder}`,
}

export const btnStyle: CSSProperties = {
  background: '#2563eb',
  color: '#fff',
  border: 'none',
  borderRadius: 6,
  padding: '8px 20px',
  fontSize: 13,
  cursor: 'pointer',
}

export const btnActiveStyle = (active: boolean): CSSProperties => ({
  padding: '6px 14px',
  borderRadius: 6,
  border: `1px solid ${active ? colors.accent : colors.cardBorder}`,
  background: active ? colors.activeBg : colors.cardBg,
  color: active ? colors.accent : colors.textSecondary,
  cursor: 'pointer',
  fontSize: 13,
})
