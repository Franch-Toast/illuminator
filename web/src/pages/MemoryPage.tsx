import { colors } from '../styles/theme'
import { EmptyState } from '../components/FeaturePanel/FeaturePanel'

export default function MemoryPage() {
  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 20 }}>
      <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>Memory</h2>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>Heap Profiling</h3>
        <EmptyState featureName="Memory Heap Profile" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: jemalloc/tcmalloc heap profiling, allocation tracking, leak detection
        </p>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>Memory Usage</h3>
        <EmptyState featureName="System Memory Usage" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: RSS/VSS tracking, page fault monitoring, NUMA statistics
        </p>
      </div>
    </div>
  )
}
