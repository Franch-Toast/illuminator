import { colors } from '../../styles/theme'

interface SummaryCardProps {
  label: string
  value: string
  color?: string
}

export default function SummaryCard({ label, value, color }: SummaryCardProps) {
  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: '12px 16px',
    }}>
      <div style={{ fontSize: 10, color: colors.textMuted, marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 18, fontWeight: 600, fontVariantNumeric: 'tabular-nums', color: color ?? colors.textPrimary }}>
        {value}
      </div>
    </div>
  )
}
