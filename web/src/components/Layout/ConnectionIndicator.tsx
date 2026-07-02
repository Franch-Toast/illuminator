import { useConnectionStatus } from '../../hooks/useDataSource'
import { colors } from '../../styles/theme'

export default function ConnectionIndicator() {
  const status = useConnectionStatus()

  const config = {
    connected: { color: colors.success, label: 'SSE Connected', icon: '●' },
    connecting: { color: colors.warnText, label: 'Connecting...', icon: '○' },
    disconnected: { color: '#6b7280', label: 'Disconnected', icon: '○' },
  }[status]

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      gap: 4,
      fontSize: 11,
      color: config.color,
    }} title={status === 'disconnected'
      ? 'SSE connection lost. Attempting reconnection...'
      : status === 'connected'
        ? 'Real-time SSE connection active'
        : 'Establishing SSE connection...'
    }>
      <span>{config.icon}</span>
      <span>{config.label}</span>
    </div>
  )
}
