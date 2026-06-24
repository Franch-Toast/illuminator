import { useFeatureHealth, FeatureHealthStatus } from '../hooks/useFeatureHealth'
import { colors } from '../styles/theme'

const statusConfig: Record<FeatureHealthStatus, { color: string; icon: string; label: string }> = {
  active: { color: colors.success, icon: '●', label: 'Data flowing' },
  degraded: { color: colors.warnText, icon: '◐', label: 'No data (>5s)' },
  unavailable: { color: '#ef4444', icon: '○', label: 'Unavailable (>15s)' },
}

interface Props {
  featureName: string
  showLabel?: boolean
}

export default function FeatureHealthBadge({ featureName, showLabel = false }: Props) {
  const health = useFeatureHealth(featureName)
  const cfg = statusConfig[health]

  return (
    <span
      style={{ display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 11, color: cfg.color }}
      title={`${featureName}: ${cfg.label}`}
    >
      <span>{cfg.icon}</span>
      {showLabel && <span>{cfg.label}</span>}
    </span>
  )
}
