import { useState, useEffect, useRef } from 'react'
import { api } from '../../services/apiClient'
import { colors } from '../../styles/theme'

export default function RecordingControl() {
  const [recording, setRecording] = useState(false)
  const [elapsed, setElapsed] = useState(0)
  const [totalBytes, setTotalBytes] = useState(0)
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const startTimeRef = useRef(0)

  useEffect(() => {
    api.recordingStatus().then(resp => {
      if (resp.recording) {
        setRecording(true)
        setTotalBytes(resp.total_bytes)
        startTimeRef.current = Date.now()
      }
    }).catch(() => {})
  }, [])

  useEffect(() => {
    if (recording) {
      startTimeRef.current = Date.now()
      timerRef.current = setInterval(() => {
        setElapsed(Math.floor((Date.now() - startTimeRef.current) / 1000))
        api.recordingStatus().then(resp => {
          setTotalBytes(resp.total_bytes)
        }).catch(() => {})
      }, 2000)
    } else {
      if (timerRef.current) clearInterval(timerRef.current)
      timerRef.current = null
      setElapsed(0)
    }
    return () => { if (timerRef.current) clearInterval(timerRef.current) }
  }, [recording])

  const toggle = async () => {
    try {
      if (recording) {
        await api.recordingStop()
        setRecording(false)
      } else {
        await api.recordingStart()
        setRecording(true)
      }
    } catch { /* ignore */ }
  }

  const formatTime = (sec: number) => {
    const m = Math.floor(sec / 60).toString().padStart(2, '0')
    const s = (sec % 60).toString().padStart(2, '0')
    return `${m}:${s}`
  }

  const formatSize = (bytes: number) => {
    if (bytes < 1024) return `${bytes}B`
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)}KB`
    return `${(bytes / 1024 / 1024).toFixed(1)}MB`
  }

  return (
    <button onClick={toggle} style={{
      display: 'flex', alignItems: 'center', gap: 6,
      padding: '6px 12px', borderRadius: 6,
      border: `1px solid ${recording ? colors.danger : colors.cardBorder}`,
      background: recording ? '#331a1a' : colors.cardBg,
      color: recording ? colors.danger : colors.textSecondary,
      cursor: 'pointer', fontSize: 11, width: '100%',
      transition: 'all 0.2s',
    }}>
      <span style={{
        width: 8, height: 8, borderRadius: '50%',
        background: recording ? colors.danger : colors.textMuted,
        animation: recording ? 'pulse 1.5s infinite' : 'none',
      }} />
      {recording ? (
        <span>{formatTime(elapsed)} | {formatSize(totalBytes)}</span>
      ) : (
        <span>Record</span>
      )}
      <style>{`@keyframes pulse { 0%,100% { opacity:1 } 50% { opacity:0.4 } }`}</style>
    </button>
  )
}
