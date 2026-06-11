import React from 'react'
import { FeatureState } from '../../hooks/useFeatureStream'
import { colors } from '../../styles/theme'

interface ControlBarProps {
  state: FeatureState
  featureName: string
  onStart: () => void
  onStop: () => void
  onPause: () => void
  onResume: () => void
  isRecording?: boolean
  onRecordToggle?: () => void
  error?: string | null
}

export function ControlBar({ state, featureName, onStart, onStop, onPause, onResume, isRecording, onRecordToggle, error }: ControlBarProps) {
  const btnBase: React.CSSProperties = {
    padding: '6px 14px',
    borderRadius: 4,
    border: 'none',
    cursor: 'pointer',
    fontSize: 13,
    fontWeight: 500,
    transition: 'opacity 0.15s',
  }

  const startBtn: React.CSSProperties = {
    ...btnBase,
    background: colors.success,
    color: '#000',
  }

  const stopBtn: React.CSSProperties = {
    ...btnBase,
    background: '#ef4444',
    color: '#fff',
  }

  const pauseBtn: React.CSSProperties = {
    ...btnBase,
    background: colors.amber,
    color: '#000',
  }

  const resumeBtn: React.CSSProperties = {
    ...btnBase,
    background: colors.accent,
    color: '#000',
  }

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 12 }}>
      <span style={{ fontSize: 14, fontWeight: 600, color: colors.textPrimary, flex: 1 }}>
        {featureName}
      </span>

      {state === 'inactive' && (
        <button style={startBtn} onClick={onStart}>Start</button>
      )}
      {state === 'active' && (
        <>
          <button style={pauseBtn} onClick={onPause}>Pause</button>
          <button style={stopBtn} onClick={onStop}>Stop</button>
        </>
      )}
      {state === 'paused' && (
        <>
          <button style={resumeBtn} onClick={onResume}>Resume</button>
          <button style={stopBtn} onClick={onStop}>Stop</button>
        </>
      )}
      {state === 'active' && onRecordToggle && (
        <button
          style={{
            ...btnBase,
            background: isRecording ? '#ef4444' : '#6b7280',
            color: '#fff',
            display: 'flex', alignItems: 'center', gap: 4,
          }}
          onClick={onRecordToggle}
        >
          <span style={{
            width: 8, height: 8, borderRadius: '50%',
            background: isRecording ? '#fff' : '#ef4444',
            animation: isRecording ? 'pulse 1.5s infinite' : 'none',
          }} />
          {isRecording ? 'Stop Rec' : 'Record'}
        </button>
      )}
      {(state === 'starting' || state === 'stopping') && (
        <span style={{ fontSize: 12, color: colors.textMuted }}>
          {state === 'starting' ? 'Starting...' : 'Stopping...'}
        </span>
      )}

      {error && (
        <span style={{ fontSize: 12, color: '#ef4444', marginLeft: 8 }}>
          {error}
        </span>
      )}

      <StateIndicator state={state} />
    </div>
  )
}

function StateIndicator({ state }: { state: FeatureState }) {
  const dotColors: Record<FeatureState, string> = {
    inactive: '#6b7280',
    starting: colors.amber,
    active: colors.success,
    paused: colors.amber,
    stopping: '#6b7280',
  }

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
      <div style={{
        width: 8, height: 8, borderRadius: '50%',
        background: dotColors[state],
        boxShadow: state === 'active' ? `0 0 6px ${colors.success}` : 'none',
      }} />
      <span style={{ fontSize: 11, color: colors.textMuted, textTransform: 'capitalize' }}>
        {state}
      </span>
    </div>
  )
}

interface EmptyStateProps {
  featureName: string
  onStart: () => void
}

export function EmptyState({ featureName, onStart }: EmptyStateProps) {
  return (
    <div style={{
      display: 'flex', flexDirection: 'column', alignItems: 'center',
      justifyContent: 'center', padding: '48px 24px',
      border: `1px dashed ${colors.cardBorder}`, borderRadius: 8,
      background: colors.bg,
    }}>
      <div style={{ fontSize: 48, marginBottom: 16, opacity: 0.4 }}>
        {'{ }'}
      </div>
      <div style={{ fontSize: 14, color: colors.textSecondary, marginBottom: 8 }}>
        {featureName} is not running
      </div>
      <div style={{ fontSize: 12, color: colors.textMuted, marginBottom: 20 }}>
        Click the button below to start data collection
      </div>
      <button
        onClick={onStart}
        style={{
          padding: '8px 20px', borderRadius: 6, border: 'none',
          background: colors.accent, color: '#fff', cursor: 'pointer',
          fontSize: 13, fontWeight: 500,
        }}
      >
        Start Collection
      </button>
    </div>
  )
}

interface FeaturePanelProps {
  featureName: string
  state: FeatureState
  error?: string | null
  onStart: () => void
  onStop: () => void
  onPause: () => void
  onResume: () => void
  isRecording?: boolean
  onRecordToggle?: () => void
  children: React.ReactNode
}

export default function FeaturePanel({
  featureName, state, error, onStart, onStop, onPause, onResume, isRecording, onRecordToggle, children
}: FeaturePanelProps) {
  return (
    <div style={{
      background: colors.cardBg,
      border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8,
      padding: 16,
    }}>
      <ControlBar
        state={state}
        featureName={featureName}
        onStart={onStart}
        onStop={onStop}
        onPause={onPause}
        onResume={onResume}
        isRecording={isRecording}
        onRecordToggle={onRecordToggle}
        error={error}
      />
      {state === 'inactive' ? (
        <EmptyState featureName={featureName} onStart={onStart} />
      ) : (
        <div style={{ opacity: state === 'paused' ? 0.6 : 1, position: 'relative' }}>
          {children}
          {state === 'paused' && (
            <div style={{
              position: 'absolute', top: '50%', left: '50%',
              transform: 'translate(-50%, -50%)',
              background: 'rgba(0,0,0,0.7)', padding: '8px 16px',
              borderRadius: 4, fontSize: 13, color: colors.textSecondary,
            }}>
              Paused
            </div>
          )}
        </div>
      )}
    </div>
  )
}
