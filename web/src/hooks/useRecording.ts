/**
 * useRecording — 录制控制 hook
 *
 * 提供两种功能：
 *   1. 前端保存：将 DataBus ringBuffer 中的数据导出为 .ilr 文件下载
 *   2. 后端录制：触发后端 RecordingSink 文件 I/O 落盘
 */

import { useState, useCallback } from 'react'
import { api } from '../services/apiClient'
import { dataBus } from '../services/dataBus'

export interface RecordingState {
  isRecording: boolean
  startTime: number | null
  feature: string | null
}

export function useRecording() {
  const [state, setState] = useState<RecordingState>({
    isRecording: false,
    startTime: null,
    feature: null,
  })

  const startRecording = useCallback(async (feature: string) => {
    try {
      await api.startRecording(feature, '/tmp/illuminator_data')
      setState({ isRecording: true, startTime: Date.now(), feature })
    } catch (e) {
      console.error('Failed to start recording:', e)
    }
  }, [])

  const stopRecording = useCallback(async () => {
    if (!state.feature) return
    try {
      await api.stopRecording(state.feature)
      setState({ isRecording: false, startTime: null, feature: null })
    } catch (e) {
      console.error('Failed to stop recording:', e)
    }
  }, [state.feature])

  return { ...state, startRecording, stopRecording }
}

/**
 * 前端保存 — 将 DataBus ringBuffer 序列化为 .ilr 文件并下载
 */
export function useSaveBuffer() {
  const [saving, setSaving] = useState(false)

  const saveToFile = useCallback(async (feature: string) => {
    setSaving(true)
    try {
      const buffer = dataBus.getBuffer(feature)
      if (buffer.length === 0) {
        alert('No data to save')
        return
      }

      const lines: string[] = []
      lines.push(JSON.stringify({
        format: 'ilr',
        version: 2,
        generated_at: Date.now(),
        generated_by: 'illuminator-web',
        features: [feature],
        window_seconds: dataBus.getWindowSize(),
      }))

      for (const batch of buffer) {
        lines.push(JSON.stringify(batch))
      }

      const blob = new Blob([lines.join('\n') + '\n'], {
        type: 'application/x-illuminator-recording'
      })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `illuminator_${feature}_${new Date().toISOString().replace(/[:.]/g, '-')}.ilr`
      document.body.appendChild(a)
      a.click()
      document.body.removeChild(a)
      URL.revokeObjectURL(url)
    } finally {
      setSaving(false)
    }
  }, [])

  return { saving, saveToFile }
}
