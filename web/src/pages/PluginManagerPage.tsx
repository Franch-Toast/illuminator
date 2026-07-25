import React, { useState, useEffect, useCallback, useRef } from 'react'
import { api, FeatureDescriptor } from '../services/apiClient'

type FeatureEntry = FeatureDescriptor & {
  is_recording?: boolean
  batches_processed?: number
  records_processed?: number
  errors?: number
  uptime_ms?: number
}
import { colors } from '../styles/theme'

const TIER_LABELS: Record<number, { label: string; color: string; desc: string }> = {
  1: { label: 'Always-On · Monitoring', color: '#4ade80', desc: 'procfs, < 0.5% CPU, daemon auto-start' },
  2: { label: 'Always-On · Tracing', color: '#60a5fa', desc: 'eBPF, 1-3% CPU, daemon auto-start' },
  3: { label: 'On-Demand · Profiling', color: '#f59e0b', desc: 'high-freq, 3-10% CPU, session-based' },
}

const CATEGORY_ICONS: Record<string, string> = {
  cpu: '🔥', memory: '🧠', io: '💾', network: '🌐',
  gpu: '🎮', scheduler: '⚙️', system: '📊',
}

const STATE_COLORS: Record<string, string> = {
  inactive: '#6b7280', starting: '#f59e0b', active: '#4ade80',
  paused: '#f59e0b', stopping: '#6b7280',
}

interface PluginAction {
  feature: string
  action: 'start' | 'stop' | 'pause' | 'resume'
}

export default function PluginManagerPage() {
  const [features, setFeatures] = useState<FeatureEntry[]>([])
  const [pendingActions, setPendingActions] = useState<Set<string>>(new Set())
  const [filter, setFilter] = useState<'all' | 'active' | 'inactive'>('all')
  const [categoryFilter, setCategoryFilter] = useState<string>('all')
  const [actionLog, setActionLog] = useState<Array<{ ts: number; msg: string; ok: boolean }>>([])
  const [reloadStatus, setReloadStatus] = useState<'idle' | 'loading' | 'success' | 'error'>('idle')
  const intervalRef = useRef<ReturnType<typeof setInterval>>()

  const fetchData = useCallback(async () => {
    try {
      const featRes = await api.features()
      setFeatures(featRes.features)
    } catch { /* backend not available */ }
  }, [])

  useEffect(() => {
    fetchData() // eslint-disable-line react-hooks/set-state-in-effect
    intervalRef.current = setInterval(fetchData, 2000)
    return () => clearInterval(intervalRef.current)
  }, [fetchData])

  const performAction = useCallback(async ({ feature, action }: PluginAction) => {
    setPendingActions(prev => new Set(prev).add(feature))
    try {
      switch (action) {
        case 'start': await api.featureStart(feature); break
        case 'stop': await api.featureStop(feature); break
        case 'pause': await api.featurePause(feature); break
        case 'resume': await api.featureResume(feature); break
      }
      setActionLog(prev => [{ ts: Date.now(), msg: `${action} ${feature}`, ok: true }, ...prev.slice(0, 19)])
      await fetchData()
    } catch (err) {
      const msg = err instanceof Error ? err.message : 'Unknown error'
      setActionLog(prev => [{ ts: Date.now(), msg: `${action} ${feature}: ${msg}`, ok: false }, ...prev.slice(0, 19)])
    } finally {
      setPendingActions(prev => { const s = new Set(prev); s.delete(feature); return s })
    }
  }, [fetchData])

  const handleReloadPlugins = useCallback(async () => {
    setReloadStatus('loading')
    try {
      await api.pluginsReload()
      setReloadStatus('success')
      setActionLog(prev => [{ ts: Date.now(), msg: 'Plugins reloaded successfully', ok: true }, ...prev.slice(0, 19)])
      await fetchData()
      setTimeout(() => setReloadStatus('idle'), 3000)
    } catch (err) {
      setReloadStatus('error')
      const msg = err instanceof Error ? err.message : 'Unknown error'
      setActionLog(prev => [{ ts: Date.now(), msg: `Plugin reload failed: ${msg}`, ok: false }, ...prev.slice(0, 19)])
      setTimeout(() => setReloadStatus('idle'), 3000)
    }
  }, [fetchData])

  const handleBatchAction = useCallback(async (action: 'start' | 'stop') => {
    const targets = features.filter(f => {
      if (f.tier <= 2) return false // Always-On features are daemon-managed
      if (action === 'start') return f.state === 'inactive'
      return f.state === 'active' || f.state === 'paused'
    })
    for (const f of targets) {
      await performAction({ feature: f.name, action })
    }
  }, [features, performAction])

  const categories = [...new Set(features.map(f => f.category))]
  const filteredFeatures = features.filter(f => {
    if (filter === 'active' && f.state === 'inactive') return false
    if (filter === 'inactive' && f.state !== 'inactive') return false
    if (categoryFilter !== 'all' && f.category !== categoryFilter) return false
    return true
  })

  const activeCount = features.filter(f => f.state === 'active' || f.state === 'paused').length
  const totalFeatures = features.length

  return (
    <div style={{ padding: 20, maxWidth: 1200, margin: '0 auto' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 16, marginBottom: 20 }}>
        <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>
          Feature Health Dashboard
        </h2>
        <span style={{ fontSize: 12, color: colors.textMuted, background: colors.cardBg, padding: '4px 10px', borderRadius: 12 }}>
          {activeCount}/{totalFeatures} active
        </span>
        <div style={{ flex: 1 }} />
        <button
          onClick={handleReloadPlugins}
          disabled={reloadStatus === 'loading'}
          style={{
            padding: '8px 16px', borderRadius: 6, border: `1px solid ${colors.cardBorder}`,
            background: reloadStatus === 'success' ? 'rgba(74,222,128,0.1)' : reloadStatus === 'error' ? 'rgba(239,68,68,0.1)' : colors.cardBg,
            color: reloadStatus === 'success' ? '#4ade80' : reloadStatus === 'error' ? '#ef4444' : colors.accent,
            cursor: reloadStatus === 'loading' ? 'wait' : 'pointer', fontSize: 13,
          }}
        >
          {reloadStatus === 'loading' ? 'Scanning...' : reloadStatus === 'success' ? 'Reloaded!' : 'Hot Reload Plugins'}
        </button>
      </div>

      {/* Filter Bar */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 16, flexWrap: 'wrap' }}>
        {(['all', 'active', 'inactive'] as const).map(f => (
          <button key={f} onClick={() => setFilter(f)} style={{
            padding: '5px 12px', borderRadius: 4, border: `1px solid ${filter === f ? colors.accent : colors.cardBorder}`,
            background: filter === f ? 'rgba(96,165,250,0.1)' : 'transparent',
            color: filter === f ? colors.accent : colors.textMuted, cursor: 'pointer', fontSize: 12,
          }}>
            {f === 'all' ? `All (${totalFeatures})` : f === 'active' ? `Active (${activeCount})` : `Inactive (${totalFeatures - activeCount})`}
          </button>
        ))}
        <span style={{ width: 1, height: 20, background: colors.cardBorder, margin: '0 4px' }} />
        <button onClick={() => setCategoryFilter('all')} style={{
          padding: '5px 10px', borderRadius: 4, border: `1px solid ${categoryFilter === 'all' ? colors.accent : colors.cardBorder}`,
          background: categoryFilter === 'all' ? 'rgba(96,165,250,0.1)' : 'transparent',
          color: categoryFilter === 'all' ? colors.accent : colors.textMuted, cursor: 'pointer', fontSize: 12,
        }}>
          All Categories
        </button>
        {categories.map(c => (
          <button key={c} onClick={() => setCategoryFilter(c)} style={{
            padding: '5px 10px', borderRadius: 4, border: `1px solid ${categoryFilter === c ? colors.accent : colors.cardBorder}`,
            background: categoryFilter === c ? 'rgba(96,165,250,0.1)' : 'transparent',
            color: categoryFilter === c ? colors.accent : colors.textMuted, cursor: 'pointer', fontSize: 12,
          }}>
            {CATEGORY_ICONS[c] || '📦'} {c}
          </button>
        ))}
        <div style={{ flex: 1 }} />
        <button onClick={() => handleBatchAction('start')} style={{
          padding: '5px 12px', borderRadius: 4, border: `1px solid ${colors.cardBorder}`,
          background: 'transparent', color: '#4ade80', cursor: 'pointer', fontSize: 12,
        }}>
          Start All On-Demand
        </button>
        <button onClick={() => handleBatchAction('stop')} style={{
          padding: '5px 12px', borderRadius: 4, border: `1px solid ${colors.cardBorder}`,
          background: 'transparent', color: '#ef4444', cursor: 'pointer', fontSize: 12,
        }}>
          Stop All On-Demand
        </button>
      </div>

      {/* Feature Cards */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(360px, 1fr))', gap: 12 }}>
        {filteredFeatures.map(f => (
          <FeatureCard
            key={f.name}
            feature={f}
            isPending={pendingActions.has(f.name)}
            onAction={(action) => performAction({ feature: f.name, action })}
          />
        ))}
      </div>

      {filteredFeatures.length === 0 && (
        <div style={{ textAlign: 'center', padding: 48, color: colors.textMuted }}>
          No features matching current filters
        </div>
      )}

      {/* Action Log */}
      {actionLog.length > 0 && (
        <div style={{
          marginTop: 24, background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
          borderRadius: 8, padding: 16, maxHeight: 200, overflowY: 'auto',
        }}>
          <div style={{ fontSize: 12, color: colors.textMuted, marginBottom: 8, fontWeight: 600 }}>
            Action Log
          </div>
          {actionLog.map((log, i) => (
            <div key={i} style={{ fontSize: 11, color: log.ok ? colors.textSecondary : '#ef4444', padding: '2px 0', fontFamily: 'monospace' }}>
              <span style={{ color: colors.textMuted }}>{new Date(log.ts).toLocaleTimeString()}</span>
              {' '}{log.ok ? '✓' : '✕'} {log.msg}
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

function FeatureCard({ feature, isPending, onAction }: {
  feature: FeatureEntry
  isPending: boolean
  onAction: (action: 'start' | 'stop' | 'pause' | 'resume') => void
}) {
  const tier = TIER_LABELS[feature.tier] || TIER_LABELS[1]
  const stateColor = STATE_COLORS[feature.state] || '#6b7280'
  const isActive = feature.state === 'active'
  const isPaused = feature.state === 'paused'
  const isAlwaysOn = feature.tier <= 2

  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`, borderRadius: 8,
      padding: 16, opacity: isPending ? 0.6 : 1, transition: 'opacity 0.2s',
      borderLeft: `3px solid ${stateColor}`,
    }}>
      <div style={{ display: 'flex', alignItems: 'flex-start', gap: 10 }}>
        <span style={{ fontSize: 20 }}>{CATEGORY_ICONS[feature.category] || '📦'}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontSize: 14, fontWeight: 600, color: colors.textPrimary }}>
              {feature.display_name || feature.name}
            </span>
            <span style={{
              fontSize: 10, padding: '2px 6px', borderRadius: 3,
              background: `${stateColor}20`, color: stateColor, fontWeight: 500,
            }}>
              {feature.state.toUpperCase()}
            </span>
            {isAlwaysOn && isActive && (
              <span style={{ fontSize: 9, padding: '1px 5px', borderRadius: 3, background: 'rgba(74,222,128,0.1)', color: '#4ade80' }}>
                AUTO
              </span>
            )}
          </div>
          <div style={{ fontSize: 11, color: colors.textMuted, marginTop: 2 }}>
            {feature.name}
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 6 }}>
            <span style={{
              fontSize: 10, padding: '1px 5px', borderRadius: 3,
              border: `1px solid ${tier.color}40`, color: tier.color,
            }}>
              {tier.label}
            </span>
            <span style={{ fontSize: 10, color: colors.textMuted }}>
              {feature.category}
            </span>
          </div>
        </div>
      </div>

      {(isActive || isPaused) && (feature.batches_processed != null || feature.records_processed != null) && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 8, marginTop: 12, paddingTop: 12, borderTop: `1px solid ${colors.cardBorder}` }}>
          <MiniStat label="Batches" value={(feature.batches_processed ?? 0).toLocaleString()} />
          <MiniStat label="Records" value={(feature.records_processed ?? 0).toLocaleString()} />
          <MiniStat label="Errors" value={String(feature.errors ?? 0)} danger={(feature.errors ?? 0) > 0} />
        </div>
      )}

      {(isActive || isPaused) && (feature.uptime_ms ?? 0) > 0 && (
        <div style={{ fontSize: 10, color: colors.textMuted, marginTop: 6 }}>
          Uptime: {formatUptime(feature.uptime_ms ?? 0)}
        </div>
      )}

      {/* Tier 1-2 (Always-On): read-only status, no control buttons */}
      {isAlwaysOn && (
        <div style={{ fontSize: 11, color: colors.textMuted, marginTop: 10, padding: '6px 10px', background: 'rgba(74,222,128,0.03)', borderRadius: 4, border: '1px solid rgba(74,222,128,0.1)' }}>
          Managed by daemon (auto-start). Use CLI or config to override.
        </div>
      )}

      {/* Tier 3 (On-Demand): full control buttons */}
      {!isAlwaysOn && (
        <div style={{ display: 'flex', gap: 6, marginTop: 12 }}>
          {feature.state === 'inactive' && (
            <ActionBtn label="Start Session" color="#4ade80" onClick={() => onAction('start')} disabled={isPending} />
          )}
          {isActive && (
            <>
              <ActionBtn label="Pause" color="#f59e0b" onClick={() => onAction('pause')} disabled={isPending} />
              <ActionBtn label="Stop" color="#ef4444" onClick={() => onAction('stop')} disabled={isPending} />
            </>
          )}
          {isPaused && (
            <>
              <ActionBtn label="Resume" color="#60a5fa" onClick={() => onAction('resume')} disabled={isPending} />
              <ActionBtn label="Stop" color="#ef4444" onClick={() => onAction('stop')} disabled={isPending} />
            </>
          )}
          {(feature.state === 'starting' || feature.state === 'stopping') && (
            <span style={{ fontSize: 11, color: colors.textMuted, padding: '6px 0' }}>
              {feature.state === 'starting' ? 'Starting...' : 'Stopping...'}
            </span>
          )}
        </div>
      )}
    </div>
  )
}

function ActionBtn({ label, color, onClick, disabled }: { label: string; color: string; onClick: () => void; disabled: boolean }) {
  return (
    <button onClick={onClick} disabled={disabled} style={{
      padding: '5px 12px', borderRadius: 4, border: `1px solid ${color}40`,
      background: `${color}10`, color, cursor: disabled ? 'not-allowed' : 'pointer',
      fontSize: 12, fontWeight: 500, opacity: disabled ? 0.5 : 1,
    }}>
      {label}
    </button>
  )
}

function MiniStat({ label, value, danger }: { label: string; value: string; danger?: boolean }) {
  return (
    <div>
      <div style={{ fontSize: 10, color: colors.textMuted }}>{label}</div>
      <div style={{ fontSize: 13, fontWeight: 600, color: danger ? '#ef4444' : colors.textPrimary, fontFamily: 'monospace' }}>
        {value}
      </div>
    </div>
  )
}

function formatUptime(ms: number): string {
  const sec = Math.floor(ms / 1000)
  if (sec < 60) return `${sec}s`
  if (sec < 3600) return `${Math.floor(sec / 60)}m ${sec % 60}s`
  return `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`
}
