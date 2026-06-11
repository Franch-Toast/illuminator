import { colors } from '../styles/theme'
import { EmptyState } from '../components/FeaturePanel/FeaturePanel'

export default function GpuPage() {
  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 20 }}>
      <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>GPU</h2>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>GPU Utilization</h3>
        <EmptyState featureName="GPU Utilization Monitor" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: GPU compute/memory utilization, kernel execution time, memory bandwidth
        </p>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>GPU Memory</h3>
        <EmptyState featureName="GPU Memory Tracker" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: GPU memory allocation/deallocation tracking, OOM prediction, buffer lifecycle
        </p>
      </div>
    </div>
  )
}
