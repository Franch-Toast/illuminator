import { colors } from '../styles/theme'
import { EmptyState } from '../components/FeaturePanel/FeaturePanel'

export default function IoPage() {
  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 20 }}>
      <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>IO</h2>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>Block IO Latency</h3>
        <EmptyState featureName="Block IO Monitor" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: bio_latency eBPF probe, IO latency histogram, device-level breakdown
        </p>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>File System</h3>
        <EmptyState featureName="File System Tracing" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: VFS operation tracing, file read/write latency, inode cache statistics
        </p>
      </div>
    </div>
  )
}
