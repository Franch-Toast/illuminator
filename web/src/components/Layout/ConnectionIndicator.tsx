import { useConnectionStatus } from '../../hooks/useDataSource'
import { colors } from '../../styles/theme'

export default function ConnectionIndicator() {
  const status = useConnectionStatus()

  const config = {
    connected: { color: colors.success, label: 'WS Connected', icon: '●' },
    connecting: { color: colors.warnText, label: 'Connecting...', icon: '○' },
    disconnected: { color: '#6b7280', label: 'HTTP Polling', icon: '↻' },
  }[status]

  return (
    <div style={{
      display: 'flex',
      alignItems: 'center',
      gap: 4,
      fontSize: 11,
      color: config.color,
    }} title={status === 'disconnected'
      ? 'WebSocket unavailable, using HTTP polling fallback (data still flowing)'
      : status === 'connected'
        ? 'Real-time WebSocket connection active'
        : 'Attempting WebSocket connection...'
    }>
      <span>{config.icon}</span>
      <span>{config.label}</span>
    </div>
  )
}
