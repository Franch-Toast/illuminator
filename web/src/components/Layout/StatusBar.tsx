import React, { useEffect, useState } from 'react'
import { usePipelineStore } from '../../stores/usePipelineStore'
import { wsManager } from '../../services/wsManager'
import { colors } from '../../styles/theme'

export default function StatusBar() {
  const { pipelines } = usePipelineStore()
  const [wsStatuses, setWsStatuses] = useState<Record<string, string>>({})

  useEffect(() => {
    const update = () => {
      const s: Record<string, string> = {}
      for (const p of pipelines) {
        s[p.name] = wsManager.getStatus(p.name)
      }
      setWsStatuses(s)
    }
    update()
    return wsManager.onStatusChange(update)
  }, [pipelines])

  const running = pipelines.filter(p => p.running).length
  const total = pipelines.length
  const wsConnected = Object.values(wsStatuses).filter(s => s === 'connected').length

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
      {wsConnected > 0 && (
        <span>
          WebSocket: <span style={{ color: '#4ade80' }}>{wsConnected} connected</span>
        </span>
      )}
    </div>
  )
}
