import React from 'react'
import { Routes, Route, NavLink } from 'react-router-dom'
import CpuOverview from './pages/CpuOverview'
import ProcessExplorer from './pages/ProcessExplorer'
import FlameGraph from './pages/FlameGraph'
import Timeline from './pages/Timeline'
import DiffView from './pages/DiffView'
import QueryConsole from './pages/QueryConsole'

const navItems = [
  { path: '/', label: 'CPU Overview', icon: '📊' },
  { path: '/processes', label: 'Processes', icon: '📋' },
  { path: '/flamegraph', label: 'Flame Graph', icon: '🔥' },
  { path: '/timeline', label: 'Timeline', icon: '📈' },
  { path: '/diff', label: 'Diff View', icon: '🔀' },
  { path: '/query', label: 'Query', icon: '🔍' },
]

export default function App() {
  return (
    <div style={{ display: 'flex', height: '100vh', fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif' }}>
      <nav style={{
        width: 220, background: '#1a1d23', color: '#e0e0e0',
        display: 'flex', flexDirection: 'column', padding: '16px 0',
        borderRight: '1px solid #2a2d35',
      }}>
        <div style={{
          padding: '0 20px 20px', borderBottom: '1px solid #2a2d35',
          marginBottom: 8
        }}>
          <h1 style={{ fontSize: 20, margin: 0, color: '#60a5fa', fontWeight: 700 }}>
            Illuminator
          </h1>
          <span style={{ fontSize: 11, color: '#888' }}>Observability Platform</span>
        </div>
        {navItems.map(item => (
          <NavLink
            key={item.path}
            to={item.path}
            end={item.path === '/'}
            style={({ isActive }) => ({
              display: 'flex', alignItems: 'center', gap: 10,
              padding: '10px 20px', textDecoration: 'none',
              color: isActive ? '#60a5fa' : '#b0b0b0',
              background: isActive ? '#252830' : 'transparent',
              borderLeft: isActive ? '3px solid #60a5fa' : '3px solid transparent',
              fontSize: 14, transition: 'all 0.15s',
            })}
          >
            <span>{item.icon}</span>
            <span>{item.label}</span>
          </NavLink>
        ))}
        <div style={{ flex: 1 }} />
        <div style={{ padding: '12px 20px', fontSize: 11, color: '#666' }}>
          v0.1.0 · C++ eBPF Engine
        </div>
      </nav>

      <main style={{ flex: 1, background: '#0f1117', color: '#e0e0e0', overflow: 'auto' }}>
        <Routes>
          <Route path="/" element={<CpuOverview />} />
          <Route path="/processes" element={<ProcessExplorer />} />
          <Route path="/flamegraph" element={<FlameGraph />} />
          <Route path="/timeline" element={<Timeline />} />
          <Route path="/diff" element={<DiffView />} />
          <Route path="/query" element={<QueryConsole />} />
        </Routes>
      </main>
    </div>
  )
}
