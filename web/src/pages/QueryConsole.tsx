import React, { useState } from 'react'

export default function QueryConsole() {
  const WIP = true
  const [query, setQuery] = useState('SELECT * FROM records ORDER BY timestamp_ns DESC LIMIT 20')
  const [results, setResults] = useState<any[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)

  const executeQuery = async () => {
    setLoading(true)
    setError(null)
    try {
      const res = await fetch('/api/v1/query', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ query }),
      })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const data = await res.json()
      setResults(data.rows || [])
    } catch (e: any) {
      setError(e.message)
      setResults([])
    }
    setLoading(false)
  }

  return (
    <div style={{ padding: 24 }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 22 }}>Query Console</h2>
      {WIP && (
        <div style={{
          background: '#332b00', border: '1px solid #665500',
          borderRadius: 8, padding: 12, marginBottom: 16,
          color: '#fbbf24', fontSize: 13,
        }}>
          Work in progress — backend /api/v1/query endpoint not yet implemented.
        </div>
      )}

      <div style={{
        background: '#1a1d23', border: '1px solid #2a2d35',
        borderRadius: 8, padding: 16, marginBottom: 16,
      }}>
        <textarea
          value={query}
          onChange={e => setQuery(e.target.value)}
          style={{
            width: '100%', height: 80, background: '#0f1117',
            color: '#e0e0e0', border: '1px solid #2a2d35',
            borderRadius: 6, padding: 12, fontFamily: 'monospace',
            fontSize: 13, resize: 'vertical',
          }}
          placeholder="Enter SQL query..."
        />
        <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 8, alignItems: 'center' }}>
          <div style={{ fontSize: 12, color: '#666' }}>
            Query the local SQLite storage. Tables: records, stack_samples, profiles
          </div>
          <button onClick={executeQuery} disabled={loading} style={{
            ...btnStyle,
            opacity: loading ? 0.6 : 1,
          }}>
            {loading ? 'Running...' : 'Execute (Ctrl+Enter)'}
          </button>
        </div>
      </div>

      {error && (
        <div style={{
          background: '#331a1a', border: '1px solid #7f1d1d',
          borderRadius: 8, padding: 12, marginBottom: 16,
          color: '#f87171', fontSize: 13,
        }}>
          Error: {error}
        </div>
      )}

      {results.length > 0 && (
        <div style={{
          background: '#1a1d23', border: '1px solid #2a2d35',
          borderRadius: 8, overflow: 'auto',
        }}>
          <table style={{ width: '100%', borderCollapse: 'collapse' }}>
            <thead>
              <tr style={{ borderBottom: '1px solid #2a2d35' }}>
                {Object.keys(results[0]).map(key => (
                  <th key={key} style={{
                    textAlign: 'left', padding: '10px 12px',
                    fontSize: 12, color: '#888', fontWeight: 600,
                  }}>{key}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {results.map((row, i) => (
                <tr key={i} style={{ borderBottom: '1px solid #1f2228' }}>
                  {Object.values(row).map((val: any, j) => (
                    <td key={j} style={{
                      padding: '8px 12px', fontSize: 13,
                      fontFamily: 'monospace', maxWidth: 300,
                      overflow: 'hidden', textOverflow: 'ellipsis',
                    }}>
                      {typeof val === 'object' ? JSON.stringify(val) : String(val)}
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
          <div style={{ padding: '8px 12px', fontSize: 12, color: '#666', borderTop: '1px solid #2a2d35' }}>
            {results.length} rows returned
          </div>
        </div>
      )}

      {results.length === 0 && !error && (
        <div style={{
          background: '#1a1d23', border: '1px solid #2a2d35',
          borderRadius: 8, padding: 40, textAlign: 'center', color: '#666',
        }}>
          Run a query to see results
        </div>
      )}
    </div>
  )
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '8px 20px', fontSize: 13, cursor: 'pointer',
}
