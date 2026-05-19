import React, { useEffect, useState } from 'react'
import { Routes, Route, NavLink, useLocation } from 'react-router-dom'
import TimeControls from './components/TimeControls/TimeControls'
import StatusBar from './components/Layout/StatusBar'
import { usePipelinePolling } from './hooks/usePipelinePolling'
import CpuOverview from './pages/CpuOverview'
import ProcessExplorer from './pages/ProcessExplorer'
import FlameGraph from './pages/FlameGraph'
import Timeline from './pages/Timeline'
import DiffView from './pages/DiffView'
import QueryConsole from './pages/QueryConsole'
import SystemPage from './pages/SystemPage'

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
  { path: '/', label: 'Dashboard', icon: '📊' },
  { path: '/processes', label: 'Processes', icon: '📋' },
  { path: '/profiler', label: 'Profiler', icon: '🔥' },
  { path: '/scheduler', label: 'Scheduler', icon: '📈' },
  { path: '/compare', label: 'Compare', icon: '🔀' },
  { path: '/query', label: 'Query', icon: '🔍' },
  { path: '/system', label: 'System', icon: '⚙️' },
]

export default function App() {
  const [version, setVersion] = useState('...')
  usePipelinePolling(3000)

  useEffect(() => {
    fetch('/healthz')
      .then(r => r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`)))
      .then(d => setVersion(d.version ?? '?'))
      .catch(() => setVersion('offline'))
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
          <h1 style={{ fontSize: 18, margin: 0, color: '#60a5fa', fontWeight: 700 }}>
            Illuminator
          </h1>
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
        <div style={{ padding: '12px 16px', fontSize: 10, color: '#555' }}>
          v{version}
        </div>
      </nav>

      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minWidth: 0 }}>
        <TimeControls />

        <main style={{ flex: 1, background: '#0f1117', color: '#e0e0e0', overflow: 'auto' }}>
          <ErrorBoundary>
            <Routes>
              <Route path="/" element={<CpuOverview />} />
              <Route path="/processes" element={<ProcessExplorer />} />
              <Route path="/profiler" element={<FlameGraph />} />
              <Route path="/scheduler" element={<Timeline />} />
              <Route path="/compare" element={<DiffView />} />
              <Route path="/query" element={<QueryConsole />} />
              <Route path="/system" element={<SystemPage />} />
            </Routes>
          </ErrorBoundary>
        </main>

        <StatusBar />
      </div>
    </div>
  )
}
