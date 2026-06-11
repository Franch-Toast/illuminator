import React, { useEffect, useState, useCallback, lazy, Suspense } from 'react'
import { Routes, Route, NavLink } from 'react-router-dom'
import TimeControls from './components/TimeControls/TimeControls'
import StatusBar from './components/Layout/StatusBar'
import { usePipelinePolling } from './hooks/usePipelinePolling'
import { useTimeStore } from './stores/useTimeStore'

const OverviewPage = lazy(() => import('./pages/OverviewPage'))
const CpuPage = lazy(() => import('./pages/CpuPage'))
const MemoryPage = lazy(() => import('./pages/MemoryPage'))
const IoPage = lazy(() => import('./pages/IoPage'))
const NetworkPage = lazy(() => import('./pages/NetworkPage'))
const GpuPage = lazy(() => import('./pages/GpuPage'))
const QueryConsole = lazy(() => import('./pages/QueryConsole'))
const SystemPage = lazy(() => import('./pages/SystemPage'))

class ErrorBoundary extends React.Component<
  { children: React.ReactNode },
  { error: Error | null }
> {
  state = { error: null as Error | null }
  static getDerivedStateFromError(error: Error) { return { error } }
  render() {
    if (this.state.error) {
      return (
        <div style={{ padding: 40, color: '#f87171' }}>
          <h2>Something went wrong</h2>
          <pre style={{ fontSize: 13, whiteSpace: 'pre-wrap' }}>
            {this.state.error.message}
          </pre>
          <button onClick={() => this.setState({ error: null })}
            style={{ marginTop: 12, padding: '8px 16px', background: '#2563eb',
                     color: '#fff', border: 'none', borderRadius: 6, cursor: 'pointer' }}>
            Retry
          </button>
        </div>
      )
    }
    return this.props.children
  }
}

const navItems = [
  { path: '/', label: 'Overview', icon: '📊' },
  { path: '/cpu', label: 'CPU', icon: '🔥' },
  { path: '/memory', label: 'Memory', icon: '🧠' },
  { path: '/io', label: 'IO', icon: '💾' },
  { path: '/network', label: 'Network', icon: '🌐' },
  { path: '/gpu', label: 'GPU', icon: '🎮' },
  { path: '/query', label: 'Query', icon: '🔍' },
  { path: '/system', label: 'System', icon: '⚙️' },
]

const WINDOW_PRESETS = [30_000, 60_000, 300_000, 900_000]

export default function App() {
  const [version, setVersion] = useState('...')
  const [showShortcuts, setShowShortcuts] = useState(false)
  usePipelinePolling(3000)

  const togglePause = useTimeStore(s => s.togglePause)
  const setWindowMs = useTimeStore(s => s.setWindowMs)
  const windowMs = useTimeStore(s => s.windowMs)

  const handleKeyDown = useCallback((e: KeyboardEvent) => {
    const tag = (e.target as HTMLElement)?.tagName
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return
    if (e.key === ' ') {
      e.preventDefault()
      togglePause()
    } else if (e.key === 't' || e.key === 'T') {
      const idx = WINDOW_PRESETS.indexOf(windowMs)
      const next = WINDOW_PRESETS[(idx + 1) % WINDOW_PRESETS.length]!
      setWindowMs(next)
    } else if (e.key === '?') {
      setShowShortcuts(prev => !prev)
    }
  }, [togglePause, setWindowMs, windowMs])

  useEffect(() => {
    document.addEventListener('keydown', handleKeyDown)
    return () => document.removeEventListener('keydown', handleKeyDown)
  }, [handleKeyDown])

  useEffect(() => {
    fetch('/healthz')
      .then(r => r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`)))
      .then(d => setVersion(d.version ?? __APP_VERSION__))
      .catch(() => setVersion(__APP_VERSION__))
  }, [])

  return (
    <div style={{ display: 'flex', height: '100vh', fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif' }}>
      <nav style={{
        width: 200, background: '#1a1d23', color: '#e0e0e0',
        display: 'flex', flexDirection: 'column', padding: '16px 0',
        borderRight: '1px solid #2a2d35', flexShrink: 0,
      }}>
        <div style={{
          padding: '0 16px 16px', borderBottom: '1px solid #2a2d35',
          marginBottom: 8,
        }}>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 6 }}>
            <h1 style={{ fontSize: 18, margin: 0, color: '#60a5fa', fontWeight: 700 }}>
              Illuminator
            </h1>
            <span style={{ fontSize: 10, color: '#6b7280', fontFamily: 'monospace' }}>
              v{version}
            </span>
          </div>
          <span style={{ fontSize: 10, color: '#888' }}>Performance Observatory</span>
        </div>

        {navItems.map(item => (
          <NavLink
            key={item.path}
            to={item.path}
            end={item.path === '/'}
            style={({ isActive }) => ({
              display: 'flex', alignItems: 'center', gap: 8,
              padding: '9px 16px', textDecoration: 'none',
              color: isActive ? '#60a5fa' : '#b0b0b0',
              background: isActive ? '#252830' : 'transparent',
              borderLeft: isActive ? '3px solid #60a5fa' : '3px solid transparent',
              fontSize: 13, transition: 'all 0.15s',
            })}
          >
            <span style={{ fontSize: 14 }}>{item.icon}</span>
            <span>{item.label}</span>
          </NavLink>
        ))}

        <div style={{ flex: 1 }} />
      </nav>

      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minWidth: 0 }}>
        <TimeControls />

        <main style={{ flex: 1, background: '#0f1117', color: '#e0e0e0', overflow: 'auto' }}>
          <ErrorBoundary>
            <Suspense fallback={<div style={{ padding: 40, color: '#6b7280' }}>Loading...</div>}>
              <Routes>
                <Route path="/" element={<OverviewPage />} />
                <Route path="/cpu" element={<CpuPage />} />
                <Route path="/memory" element={<MemoryPage />} />
                <Route path="/io" element={<IoPage />} />
                <Route path="/network" element={<NetworkPage />} />
                <Route path="/gpu" element={<GpuPage />} />
                <Route path="/query" element={<QueryConsole />} />
                <Route path="/system" element={<SystemPage />} />
              </Routes>
            </Suspense>
          </ErrorBoundary>
        </main>

        <StatusBar />
      </div>

      {showShortcuts && (
        <div style={{
          position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.6)',
          display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000,
        }} onClick={() => setShowShortcuts(false)}>
          <div style={{
            background: '#1e2028', border: '1px solid #2a2d35', borderRadius: 12,
            padding: 24, minWidth: 300,
          }} onClick={e => e.stopPropagation()}>
            <h3 style={{ margin: '0 0 16px', fontSize: 16, color: '#60a5fa' }}>Keyboard Shortcuts</h3>
            {[
              ['Space', 'Toggle Live / Pause'],
              ['T', 'Cycle time window (30s → 1m → 5m → 15m)'],
              ['?', 'Show / hide this help'],
            ].map(([key, desc]) => (
              <div key={key} style={{ display: 'flex', justifyContent: 'space-between', padding: '6px 0', fontSize: 13 }}>
                <kbd style={{
                  background: '#252830', border: '1px solid #3a3d45', borderRadius: 4,
                  padding: '2px 8px', fontFamily: 'monospace', fontSize: 12,
                }}>{key}</kbd>
                <span style={{ color: '#b0b0b0' }}>{desc}</span>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}
