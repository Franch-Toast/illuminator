import { colors } from '../styles/theme'
import { EmptyState } from '../components/FeaturePanel/FeaturePanel'

export default function NetworkPage() {
  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 20 }}>
      <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>Network</h2>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>TCP Connections</h3>
        <EmptyState featureName="Network TCP Tracer" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: TCP connection lifecycle tracking, retransmit analysis, RTT measurement
        </p>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24,
      }}>
        <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.textSecondary }}>Network Latency</h3>
        <EmptyState featureName="Network Latency Monitor" onStart={() => {}} />
        <p style={{ marginTop: 12, fontSize: 12, color: colors.textMuted }}>
          Planned: Socket read/write latency, DNS resolution time, connection establishment time
        </p>
      </div>
    </div>
  )
}
