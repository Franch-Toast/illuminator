import { colors } from '../../styles/theme'

export type FeatureState = 'active' | 'paused' | 'inactive' | 'error'

const stateConfig: Record<FeatureState, { color: string; label: string; pulse?: boolean }> = {
  active: { color: colors.success, label: '运行中', pulse: true },
  paused: { color: colors.amber, label: '已暂停' },
  inactive: { color: colors.gray, label: '未启动' },
  error: { color: colors.red, label: '异常' },
}

interface StateIndicatorProps {
  state: FeatureState
}

export default function StateIndicator({ state }: StateIndicatorProps) {
  const cfg = stateConfig[state]
  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6, fontSize: 12 }}>
      <span style={{
        width: 8, height: 8, borderRadius: '50%',
        background: cfg.color,
        animation: cfg.pulse ? 'state-indicator-pulse 1.5s ease-in-out infinite' : undefined,
        boxShadow: cfg.pulse ? `0 0 6px ${cfg.color}` : 'none',
      }} />
      <span style={{ color: cfg.color, fontWeight: 500 }}>{cfg.label}</span>
      <style>{`
        @keyframes state-indicator-pulse {
          0%, 100% { opacity: 1; }
          50% { opacity: 0.4; }
        }
      `}</style>
    </span>
  )
}
