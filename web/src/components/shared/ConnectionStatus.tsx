import { useEffect, useRef, useState } from 'react'
import { useConnectionStatus } from '../../hooks/useDataSource'
import { dataBus } from '../../services/dataBus'
import { colors } from '../../styles/theme'

const statusLabels: Record<string, { label: string; color: string; blink?: boolean }> = {
  connected: { label: '已连接', color: colors.success },
  reconnecting: { label: '重连中', color: colors.amber, blink: true },
  stale: { label: '等待数据...', color: colors.amber },
  error: { label: '连接异常', color: colors.red },
  disconnected: { label: '已断开', color: colors.red },
  connecting: { label: '连接中', color: colors.warnText },
}

export default function ConnectionStatus() {
  const status = useConnectionStatus()
  const [countdown, setCountdown] = useState<number | null>(null)
  const [tick, setTick] = useState(0)
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    intervalRef.current = setInterval(() => {
      setTick((t) => t + 1)
      if (status === 'reconnecting') {
        const { nextReconnectAt } = dataBus.getConnectionStats()
        setCountdown(nextReconnectAt ? Math.max(0, nextReconnectAt - Date.now()) : null)
      } else {
        setCountdown(null)
      }
    }, 500)
    return () => {
      if (intervalRef.current) clearInterval(intervalRef.current)
    }
  }, [status])

  const config = statusLabels[status] ?? statusLabels.disconnected
  const seconds = countdown != null ? Math.max(0, Math.ceil(countdown / 1000)) : null

  return (
    <div style={{
      display: 'inline-flex', alignItems: 'center', gap: 8,
      padding: '4px 10px', borderRadius: 6,
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      fontSize: 12, color: colors.textSecondary,
    }}>
      <span style={{
        width: 8, height: 8, borderRadius: '50%',
        background: config.color,
        animation: config.blink ? 'sse-pulse 1.2s infinite' : undefined,
      }} />
      <span style={{ color: config.color, fontWeight: 500 }}>{config.label}</span>
      {status === 'reconnecting' && seconds != null && (
        <span style={{ color: colors.textMuted }}>({seconds}s后重试)</span>
      )}
      {(status === 'disconnected' || status === 'error') && (
        <button
          type="button"
          onClick={() => dataBus.reconnectNow()}
          style={{
            marginLeft: 4, padding: '2px 8px', fontSize: 11,
            background: colors.blue, color: '#fff', border: 'none',
            borderRadius: 4, cursor: 'pointer',
          }}
        >
          立即重连
        </button>
      )}
      <style>{`
        @keyframes sse-pulse {
          0%, 100% { opacity: 1; }
          50% { opacity: 0.35; }
        }
      `}</style>
    </div>
  )
}
