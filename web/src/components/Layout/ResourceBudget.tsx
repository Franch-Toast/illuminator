import { useResourceBudget } from '../../hooks/useDataSource'
import { colors } from '../../styles/theme'

export default function ResourceBudget() {
  const budget = useResourceBudget()

  if (!budget) return null

  const { usage, limits } = budget
  const memPct = (usage.rss_bytes / limits.max_memory_bytes) * 100
  const cpuPct = (usage.cpu_pct / limits.max_cpu_pct) * 100
  const overallPct = Math.max(memPct, cpuPct)

  const statusColor = overallPct > 80 ? colors.danger
    : overallPct > 50 ? colors.warnText
    : colors.success

  const formatMB = (bytes: number) => `${(bytes / 1024 / 1024).toFixed(0)}MB`

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      gap: 8,
      padding: '4px 10px',
      borderRadius: 6,
      background: colors.cardBg,
      border: `1px solid ${colors.cardBorder}`,
      fontSize: 11,
      color: colors.textSecondary,
    }}>
      <div style={{
        width: 6,
        height: 6,
        borderRadius: '50%',
        background: statusColor,
      }} />
      <span>{usage.cpu_pct.toFixed(1)}% CPU</span>
      <span style={{ color: colors.textDimmed }}>|</span>
      <span>{formatMB(usage.rss_bytes)}</span>
      <span style={{ color: colors.textDimmed }}>|</span>
      <span>Probes: {usage.ebpf_probes}</span>
    </div>
  )
}
