import { usePipelineStore } from '../../stores/usePipelineStore'
import { colors } from '../../styles/theme'

export default function StatusBar() {
  const { pipelines } = usePipelineStore()

  const running = pipelines.filter(p => p.running).length
  const total = pipelines.length

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 16,
      padding: '4px 16px',
      background: '#15171c',
      borderTop: `1px solid ${colors.cardBorder}`,
      fontSize: 11, color: colors.textMuted,
    }}>
      <span>
        Pipelines: <span style={{ color: running === total ? '#4ade80' : colors.amber }}>
          {running}/{total} running
        </span>
      </span>
    </div>
  )
}
