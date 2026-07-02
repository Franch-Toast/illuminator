/**
 * RecordingControls — 录制与保存控制栏
 *
 * 显示录制按钮（后端录制）和保存按钮（前端 buffer 导出）。
 */

import { useRecording, useSaveBuffer } from '../hooks/useRecording'
const colors = {
  surface: '#1a1d23',
  border: '#2a2d35',
  background: '#0f1117',
  textPrimary: '#e0e0e0',
  textSecondary: '#b0b0b0',
  primary: '#60a5fa',
  error: '#ef4444',
}

interface Props {
  feature?: string
  featureName?: string
  compact?: boolean
}

export default function RecordingControls({ feature, featureName, compact }: Props) {
  const resolvedFeature = feature || featureName || 'cpu_utilization'
  const { isRecording, startRecording, stopRecording } = useRecording()
  const { saving, saveToFile } = useSaveBuffer()

  const handleRecordToggle = async () => {
    if (isRecording) {
      await stopRecording()
    } else {
      await startRecording(resolvedFeature)
    }
  }

  return (
    <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
      <button
        onClick={handleRecordToggle}
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: 6,
          padding: '6px 12px',
          border: 'none',
          borderRadius: 6,
          cursor: 'pointer',
          fontSize: 13,
          fontWeight: 500,
          background: isRecording ? colors.error : colors.surface,
          color: isRecording ? '#fff' : colors.textPrimary,
          transition: 'all 0.2s',
        }}
      >
        <span style={{
          width: 8,
          height: 8,
          borderRadius: '50%',
          background: isRecording ? '#fff' : colors.error,
          animation: isRecording ? 'pulse 1s infinite' : 'none',
        }} />
        {isRecording ? 'Stop Recording' : 'Record'}
      </button>

      {!compact && <button
        onClick={() => saveToFile(resolvedFeature)}
        disabled={saving}
        style={{
          padding: '6px 12px',
          border: `1px solid ${colors.border}`,
          borderRadius: 6,
          cursor: saving ? 'not-allowed' : 'pointer',
          fontSize: 13,
          fontWeight: 500,
          background: colors.surface,
          color: colors.textPrimary,
          opacity: saving ? 0.5 : 1,
        }}
      >
        {saving ? 'Saving...' : 'Save to File'}
      </button>}
    </div>
  )
}
