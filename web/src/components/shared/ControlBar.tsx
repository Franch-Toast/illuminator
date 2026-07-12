import { useState, type ReactNode, type CSSProperties } from 'react'
import { api } from '../../services/apiClient'
import { colors } from '../../styles/theme'
import RecordingControls from '../RecordingControls'

export type DriverState = 'inactive' | 'active' | 'paused' | 'error'

interface ControlBarProps {
  /** Feature 标识名 */
  featureName: string
  /** 当前驱动状态 */
  state: DriverState
  /** 自定义启动处理（不传则使用 apiClient 默认调用） */
  onStart?: () => void | Promise<void>
  /** 自定义暂停处理 */
  onPause?: () => void | Promise<void>
  /** 自定义恢复处理 */
  onResume?: () => void | Promise<void>
  /** 自定义停止处理 */
  onStop?: () => void | Promise<void>
  /** 是否显示录制控制（默认 true） */
  showRecording?: boolean
  /** 追加自定义按钮 */
  extra?: ReactNode
}

const btnBase: CSSProperties = {
  padding: '6px 14px',
  borderRadius: 6,
  fontSize: 12,
  fontWeight: 500,
  border: 'none',
  cursor: 'pointer',
  transition: 'all 0.15s',
}

function btnStyle(color: string, busy: boolean): CSSProperties {
  return {
    ...btnBase,
    border: `1px solid ${color}55`,
    background: `${color}1a`,
    color,
    cursor: busy ? 'wait' : 'pointer',
    opacity: busy ? 0.6 : 1,
  }
}

/**
 * ControlBar — Feature 控制栏
 *
 * 根据当前 DriverState 显示可用按钮：
 *   - inactive → [启动]
 *   - active   → [暂停] [停止] [录制]
 *   - paused    → [恢复] [停止]
 *   - error     → [重试启动]
 *
 * 默认调用 apiClient 对应的 REST API，也可通过 props 自定义处理函数。
 */
export default function ControlBar({
  featureName,
  state,
  onStart,
  onPause,
  onResume,
  onStop,
  showRecording = true,
  extra,
}: ControlBarProps) {
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const runAction = async (
    defaultAction: () => Promise<unknown>,
    customHandler?: () => void | Promise<void>,
  ) => {
    setBusy(true)
    setError(null)
    try {
      if (customHandler) {
        await customHandler()
      } else {
        await defaultAction()
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      setError(msg)
    } finally {
      setBusy(false)
    }
  }

  const handleStart = () =>
    runAction(() => api.featureStart(featureName), onStart)

  const handlePause = () =>
    runAction(() => api.featurePause(featureName), onPause)

  const handleResume = () =>
    runAction(() => api.featureResume(featureName), onResume)

  const handleStop = () =>
    runAction(() => api.featureStop(featureName), onStop)

  return (
    <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
      {state === 'inactive' && (
        <button onClick={handleStart} disabled={busy} style={btnStyle(colors.success, busy)}>
          启动
        </button>
      )}

      {state === 'active' && (
        <>
          <button onClick={handlePause} disabled={busy} style={btnStyle(colors.amber, busy)}>
            暂停
          </button>
          <button onClick={handleStop} disabled={busy} style={btnStyle(colors.red, busy)}>
            停止
          </button>
          {showRecording && (
            <RecordingControls featureName={featureName} compact />
          )}
        </>
      )}

      {state === 'paused' && (
        <>
          <button onClick={handleResume} disabled={busy} style={btnStyle(colors.success, busy)}>
            恢复
          </button>
          <button onClick={handleStop} disabled={busy} style={btnStyle(colors.red, busy)}>
            停止
          </button>
        </>
      )}

      {state === 'error' && (
        <button onClick={handleStart} disabled={busy} style={btnStyle(colors.accent, busy)}>
          重试启动
        </button>
      )}

      {extra}

      {error && (
        <span style={{ fontSize: 11, color: colors.danger, maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {error}
        </span>
      )}
    </div>
  )
}
