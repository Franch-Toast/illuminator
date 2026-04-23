import React, { useState, useEffect } from 'react'
import { usePipelines, useHealth } from '../hooks/useApi'

const cardStyle: React.CSSProperties = {
  background: '#1a1d23', borderRadius: 8, padding: 20,
  border: '1px solid #2a2d35',
}

const statStyle: React.CSSProperties = {
  fontSize: 28, fontWeight: 700, color: '#60a5fa',
}

export default function Dashboard() {
  const { pipelines, error } = usePipelines(2000)
  const health = useHealth()
  const [uptime, setUptime] = useState(0)

  useEffect(() => {
    const t = setInterval(() => setUptime(u => u + 1), 1000)
    return () => clearInterval(t)
  }, [])

  const totalRecords = pipelines.reduce((s, p) => s + p.records, 0)
  const totalBatches = pipelines.reduce((s, p) => s + p.batches, 0)
  const totalErrors = pipelines.reduce((s, p) => s + p.errors, 0)

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 24 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>System Dashboard</h2>
        <div style={{
          display: 'flex', alignItems: 'center', gap: 8,
          padding: '6px 12px', borderRadius: 16,
          background: health ? '#0d331a' : '#331a1a',
          color: health ? '#4ade80' : '#f87171', fontSize: 13,
        }}>
          <span style={{
            width: 8, height: 8, borderRadius: '50%',
            background: health ? '#4ade80' : '#f87171',
          }} />
          {health ? 'Connected' : 'Disconnected'}
        </div>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 16, marginBottom: 24 }}>
        <div style={cardStyle}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Active Pipelines</div>
          <div style={statStyle}>{pipelines.filter(p => p.running).length}</div>
        </div>
        <div style={cardStyle}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Total Records</div>
          <div style={statStyle}>{totalRecords.toLocaleString()}</div>
        </div>
        <div style={cardStyle}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Batches Processed</div>
          <div style={statStyle}>{totalBatches.toLocaleString()}</div>
        </div>
        <div style={cardStyle}>
          <div style={{ fontSize: 13, color: '#888', marginBottom: 8 }}>Errors</div>
          <div style={{ ...statStyle, color: totalErrors > 0 ? '#f87171' : '#4ade80' }}>
            {totalErrors}
          </div>
        </div>
      </div>

      <div style={cardStyle}>
        <h3 style={{ margin: '0 0 16px', fontSize: 16 }}>Pipeline Status</h3>
        <table style={{ width: '100%', borderCollapse: 'collapse' }}>
          <thead>
            <tr style={{ borderBottom: '1px solid #2a2d35' }}>
              <th style={thStyle}>Name</th>
              <th style={thStyle}>Status</th>
              <th style={thStyle}>Records</th>
              <th style={thStyle}>Batches</th>
              <th style={thStyle}>Errors</th>
            </tr>
          </thead>
          <tbody>
            {pipelines.map(p => (
              <tr key={p.name} style={{ borderBottom: '1px solid #1f2228' }}>
                <td style={tdStyle}>{p.name}</td>
                <td style={tdStyle}>
                  <span style={{
                    padding: '3px 10px', borderRadius: 12, fontSize: 12,
                    background: p.running ? '#0d331a' : '#331a1a',
                    color: p.running ? '#4ade80' : '#f87171',
                  }}>
                    {p.running ? 'Running' : 'Stopped'}
                  </span>
                </td>
                <td style={tdStyle}>{p.records.toLocaleString()}</td>
                <td style={tdStyle}>{p.batches.toLocaleString()}</td>
                <td style={{ ...tdStyle, color: p.errors > 0 ? '#f87171' : '#888' }}>
                  {p.errors}
                </td>
              </tr>
            ))}
            {pipelines.length === 0 && (
              <tr><td colSpan={5} style={{ ...tdStyle, textAlign: 'center', color: '#666' }}>
                {error ? `Error: ${error}` : 'No pipelines configured'}
              </td></tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

const thStyle: React.CSSProperties = {
  textAlign: 'left', padding: '10px 12px', fontSize: 12,
  color: '#888', fontWeight: 600, textTransform: 'uppercase',
}

const tdStyle: React.CSSProperties = {
  padding: '12px', fontSize: 14,
}
