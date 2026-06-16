import { useState, useRef, useCallback, useEffect, lazy, Suspense } from 'react'
import { colors, card } from '../styles/theme'
import { ReplayEngine, type ReplayMeta, type PlaybackState } from '../services/replayEngine'
import SubTabBar from '../components/SubTabBar'

const ReplayCpuView = lazy(() => import('./replay/ReplayCpuView'))
const ReplayMemoryView = lazy(() => import('./replay/ReplayMemoryView'))

const FEATURE_TABS = [
  { id: 'cpu', label: 'CPU' },
  { id: 'memory', label: 'Memory' },
  { id: 'io', label: 'IO' },
  { id: 'network', label: 'Network' },
  { id: 'gpu', label: 'GPU' },
]

export default function ReplayPage() {
  const [engine] = useState(() => new ReplayEngine())
  const [meta, setMeta] = useState<ReplayMeta | null>(null)
  const [playState, setPlayState] = useState<PlaybackState>('idle')
  const [progress, setProgress] = useState(0)
  const [speed, setSpeed] = useState(1)
  const [activeTab, setActiveTab] = useState('cpu')
  const animRef = useRef<number | null>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    const unsub = engine.onStateChange(setPlayState)
    return () => { unsub(); engine.destroy() }
  }, [engine])

  useEffect(() => {
    if (playState === 'playing') {
      const update = () => {
        setProgress(engine.getProgress())
        animRef.current = requestAnimationFrame(update)
      }
      animRef.current = requestAnimationFrame(update)
    } else {
      if (animRef.current) cancelAnimationFrame(animRef.current)
      setProgress(engine.getProgress()) // eslint-disable-line react-hooks/set-state-in-effect
    }
    return () => { if (animRef.current) cancelAnimationFrame(animRef.current) }
  }, [playState, engine])

  const handleFile = useCallback(async (file: File) => {
    const m = await engine.loadFile(file)
    setMeta(m)
    const featureSet = new Set(m.features)
    if (featureSet.has('cpu_utilization') || featureSet.has('cpu_processes')) {
      setActiveTab('cpu')
    } else if (featureSet.has('memory_utilization')) {
      setActiveTab('memory')
    }
  }, [engine])

  const onDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault()
    const file = e.dataTransfer.files[0]
    if (file) handleFile(file)
  }, [handleFile])

  const onFileSelect = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    if (file) handleFile(file)
  }, [handleFile])

  const handleSeek = (e: React.MouseEvent<HTMLDivElement>) => {
    if (!meta) return
    const rect = e.currentTarget.getBoundingClientRect()
    const pct = (e.clientX - rect.left) / rect.width
    const ts = meta.startTs + pct * (meta.endTs - meta.startTs)
    engine.seek(ts)
    setProgress(pct)
  }

  const handleSpeedChange = (s: number) => {
    setSpeed(s)
    engine.setSpeed(s)
  }

  const formatDuration = (ms: number) => {
    const sec = Math.floor(ms / 1000)
    const m = Math.floor(sec / 60).toString().padStart(2, '0')
    const s = (sec % 60).toString().padStart(2, '0')
    return `${m}:${s}`
  }

  if (!meta) {
    return (
      <div style={{ padding: 40, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 24 }}>
        <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>Replay Mode</h2>
        <p style={{ margin: 0, fontSize: 13, color: colors.textMuted, maxWidth: 450, textAlign: 'center' }}>
          Load an Illuminator recording file (.ilr) to replay and analyze historical performance data
          using the same charts as live mode.
        </p>
        <div
          onDrop={onDrop}
          onDragOver={e => e.preventDefault()}
          style={{
            ...card,
            width: '100%', maxWidth: 500,
            padding: 48, textAlign: 'center',
            border: `2px dashed ${colors.cardBorder}`,
            cursor: 'pointer',
          }}
          onClick={() => fileInputRef.current?.click()}
        >
          <div style={{ fontSize: 32, marginBottom: 12 }}>&#128194;</div>
          <p style={{ margin: '0 0 8px', color: colors.textPrimary, fontSize: 14 }}>
            Drop .ilr file here or click to select
          </p>
          <p style={{ margin: 0, color: colors.textMuted, fontSize: 12 }}>
            Supports .ilr, .json, .ndjson formats
          </p>
          <input
            ref={fileInputRef}
            type="file"
            accept=".ilr,.json,.ndjson"
            style={{ display: 'none' }}
            onChange={onFileSelect}
          />
        </div>
      </div>
    )
  }

  const availableTabs = FEATURE_TABS.filter(tab => {
    const featureSet = new Set(meta.features)
    switch (tab.id) {
      case 'cpu': return featureSet.has('cpu_utilization') || featureSet.has('cpu_processes')
      case 'memory': return featureSet.has('memory_utilization') || featureSet.has('memory_processes')
      case 'io': return featureSet.has('io_monitor')
      case 'network': return featureSet.has('net_tracer')
      case 'gpu': return featureSet.has('gpu_monitor')
      default: return false
    }
  })

  const currentTime = meta.startTs + progress * (meta.endTs - meta.startTs)

  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 16 }}>
      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>Replay</h2>
        <span style={{
          fontSize: 11, color: colors.textMuted,
          background: colors.cardBg, padding: '2px 8px', borderRadius: 4,
          border: `1px solid ${colors.cardBorder}`,
        }}>
          {meta.features.length} features | {meta.frameCount} frames | {(meta.fileSizeBytes / 1024 / 1024).toFixed(1)}MB
        </span>
        <button
          onClick={() => { setMeta(null); engine.destroy() }}
          style={{
            marginLeft: 'auto', padding: '4px 12px', borderRadius: 4,
            border: `1px solid ${colors.cardBorder}`, background: 'transparent',
            color: colors.textMuted, fontSize: 11, cursor: 'pointer',
          }}
        >
          Load Different File
        </button>
      </div>

      {/* Feature tabs */}
      {availableTabs.length > 1 && (
        <SubTabBar
          tabs={availableTabs}
          active={activeTab}
          onChange={setActiveTab}
        />
      )}

      {/* Chart area */}
      <Suspense fallback={<div style={{ padding: 40, textAlign: 'center', color: colors.textMuted }}>Loading...</div>}>
        {activeTab === 'cpu' && <ReplayCpuView engine={engine} />}
        {activeTab === 'memory' && <ReplayMemoryView engine={engine} />}
        {activeTab === 'io' && <ReplayPlaceholder feature="IO" />}
        {activeTab === 'network' && <ReplayPlaceholder feature="Network" />}
        {activeTab === 'gpu' && <ReplayPlaceholder feature="GPU" />}
      </Suspense>

      {/* Playback control bar */}
      <div style={{
        ...card, padding: '12px 16px',
        display: 'flex', alignItems: 'center', gap: 12,
        position: 'sticky', bottom: 0,
      }}>
        <button
          onClick={() => playState === 'playing' ? engine.pause() : engine.play()}
          style={{
            background: 'none', border: 'none', cursor: 'pointer',
            fontSize: 18, color: colors.textPrimary, padding: '0 4px',
          }}
        >
          {playState === 'playing' ? '\u23F8' : '\u25B6'}
        </button>

        <span style={{ fontSize: 11, color: colors.textSecondary, minWidth: 90, fontFamily: 'monospace' }}>
          {formatDuration(currentTime - meta.startTs)} / {formatDuration(meta.endTs - meta.startTs)}
        </span>

        {/* Progress bar */}
        <div
          onClick={handleSeek}
          style={{
            flex: 1, height: 6, background: '#2a2d35', borderRadius: 3,
            cursor: 'pointer', position: 'relative',
          }}
        >
          <div style={{
            width: `${progress * 100}%`, height: '100%',
            background: colors.accent, borderRadius: 3, transition: 'width 0.1s',
          }} />
          <div style={{
            position: 'absolute', top: -3, left: `${progress * 100}%`,
            width: 12, height: 12, borderRadius: '50%',
            background: colors.accent, transform: 'translateX(-50%)',
          }} />
        </div>

        {/* Speed selector */}
        <div style={{ display: 'flex', gap: 4 }}>
          {[0.5, 1, 2, 4].map(s => (
            <button
              key={s}
              onClick={() => handleSpeedChange(s)}
              style={{
                padding: '2px 8px', borderRadius: 4, fontSize: 10,
                border: `1px solid ${speed === s ? colors.accent : colors.cardBorder}`,
                background: speed === s ? colors.activeBg : 'transparent',
                color: speed === s ? colors.accent : colors.textMuted,
                cursor: 'pointer',
              }}
            >
              {s}x
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}

function ReplayPlaceholder({ feature }: { feature: string }) {
  return (
    <div style={{
      ...card, minHeight: 200, display: 'flex',
      alignItems: 'center', justifyContent: 'center',
    }}>
      <span style={{ color: colors.textMuted, fontSize: 13 }}>
        {feature} replay view coming soon
      </span>
    </div>
  )
}
