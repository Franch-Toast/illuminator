import React, { useState, useCallback } from 'react'
import { api } from '../services/apiClient'
import { colors } from '../styles/theme'

const card: React.CSSProperties = {
  background: colors.cardBg, borderRadius: 8, padding: 16,
  border: `1px solid ${colors.cardBorder}`,
}

const EXAMPLE_QUERIES = [
  'SELECT * FROM records ORDER BY timestamp_ns DESC LIMIT 20',
  'SELECT COUNT(*) as cnt FROM records',
  'SELECT * FROM stack_samples LIMIT 10',
]

export default function QueryConsole() {
  const [query, setQuery] = useState(EXAMPLE_QUERIES[0]!)
  const [results, setResults] = useState<Record<string, unknown>[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [elapsed, setElapsed] = useState<number | null>(null)

  const executeQuery = useCallback(async () => {
    setLoading(true)
    setError(null)
    const start = performance.now()
    try {
      const data = await api.query(query)
      setResults(data.rows || [])
      setElapsed(Math.round(performance.now() - start))
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
      setResults([])
      setElapsed(null)
    }
    setLoading(false)
  }, [query])

  const handleKeyDown = useCallback((e: React.KeyboardEvent) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault()
      executeQuery()
    }
  }, [executeQuery])

  return (
    <div style={{ padding: 20 }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 20 }}>Query Console</h2>

      <div style={{ ...card, marginBottom: 16 }}>
        <textarea
          value={query}
          onChange={e => setQuery(e.target.value)}
          onKeyDown={handleKeyDown}
          style={{
            width: '100%', height: 80, background: colors.bg,
            color: colors.textPrimary, border: `1px solid ${colors.cardBorder}`,
            borderRadius: 6, padding: 12, fontFamily: 'monospace',
            fontSize: 13, resize: 'vertical',
          }}
          placeholder="Enter SQL query..."
        />
        <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 8, alignItems: 'center', flexWrap: 'wrap', gap: 8 }}>
          <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
            <span style={{ fontSize: 11, color: '#666' }}>Examples:</span>
            {EXAMPLE_QUERIES.map((q, i) => (
              <button key={i} onClick={() => setQuery(q)} style={{
                padding: '2px 8px', borderRadius: 4, fontSize: 10,
                background: 'transparent', border: `1px solid ${colors.cardBorder}`,
                color: colors.textMuted, cursor: 'pointer',
              }}>
                {q.slice(0, 30)}...
              </button>
            ))}
          </div>
          <button onClick={executeQuery} disabled={loading} style={{
            background: '#2563eb', color: '#fff', border: 'none',
            borderRadius: 6, padding: '6px 16px', fontSize: 12, cursor: 'pointer',
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
          {error}
        </div>
      )}

      {results.length > 0 && (
        <div style={{ ...card, padding: 0, overflow: 'auto' }}>
          <table style={{ width: '100%', borderCollapse: 'collapse' }}>
            <thead>
              <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
                {Object.keys(results[0]).map(key => (
                  <th key={key} style={{
                    textAlign: 'left', padding: '8px 10px',
                    fontSize: 11, color: '#888', fontWeight: 600,
                  }}>{key}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {results.map((row, i) => (
                <tr key={i} style={{ borderBottom: '1px solid #1f2228' }}>
                  {Object.values(row).map((val: unknown, j) => (
                    <td key={j} style={{
                      padding: '6px 10px', fontSize: 12,
                      fontFamily: 'monospace', maxWidth: 250,
                      overflow: 'hidden', textOverflow: 'ellipsis',
                    }}>
                      {typeof val === 'object' ? JSON.stringify(val) : String(val)}
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
          <div style={{ padding: '6px 10px', fontSize: 11, color: '#666', borderTop: `1px solid ${colors.cardBorder}` }}>
            {results.length} rows{elapsed !== null ? ` · ${elapsed}ms` : ''}
          </div>
        </div>
      )}

      {results.length === 0 && !error && (
        <div style={{ ...card, textAlign: 'center', color: '#555', padding: 40, fontSize: 13 }}>
          Run a query to see results
        </div>
      )}
    </div>
  )
}
