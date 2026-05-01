import React, { useMemo, useState } from 'react'
import { useProcessCpu, type ProcessInfo } from '../hooks/useCpuMetrics'

const card: React.CSSProperties = {
  background: '#1a1d23',
  borderRadius: 8,
  padding: 20,
  border: '1px solid #2a2d35',
}

const th: React.CSSProperties = {
  textAlign: 'left',
  padding: '10px 12px',
  fontSize: 11,
  textTransform: 'uppercase',
  letterSpacing: '0.06em',
  color: '#888',
  borderBottom: '1px solid #2a2d35',
  cursor: 'pointer',
  userSelect: 'none',
}

const td: React.CSSProperties = {
  padding: '10px 12px',
  fontSize: 13,
  borderBottom: '1px solid #252830',
}

export function formatRssKb(kb: number): string {
  if (kb >= 1024 * 1024) return `${(kb / (1024 * 1024)).toFixed(2)} GB`
  if (kb >= 1024) return `${(kb / 1024).toFixed(1)} MB`
  return `${kb} KB`
}

type SortKey = 'cpu' | 'pid' | 'name'

export default function ProcessExplorer() {
  const { processes, error } = useProcessCpu(2000)
  const [query, setQuery] = useState('')
  const [sort, setSort] = useState<SortKey>('cpu')
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set())

  const filteredSorted = useMemo(() => {
    const q = query.trim().toLowerCase()
    let list = processes.filter((p) => !q || p.comm.toLowerCase().includes(q) || p.pid.includes(q))
    list = [...list]
    if (sort === 'cpu') list.sort((a, b) => b.cpu_total_pct - a.cpu_total_pct)
    else if (sort === 'pid') list.sort((a, b) => parseInt(a.pid, 10) - parseInt(b.pid, 10))
    else list.sort((a, b) => a.comm.localeCompare(b.comm))
    return list
  }, [processes, query, sort])

  const toggle = (pid: string) => {
    setExpanded((prev) => {
      const next = new Set(prev)
      if (next.has(pid)) next.delete(pid)
      else next.add(pid)
      return next
    })
  }

  const headerBtn = (label: string, key: SortKey) => (
    <th style={th} onClick={() => setSort(key)} title={`Sort by ${label}`}>
      {label}
      {sort === key ? ' \u25BC' : ''}
    </th>
  )

  return (
    <div style={{ padding: 24 }}>
      <h2 style={{ margin: '0 0 20px', fontSize: 22 }}>Process explorer</h2>

      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 16, marginBottom: 20, alignItems: 'center' }}>
        <input
          type="search"
          placeholder="Filter by name or PID…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          style={{
            flex: '1 1 240px',
            maxWidth: 400,
            padding: '10px 14px',
            borderRadius: 8,
            border: '1px solid #2a2d35',
            background: '#252830',
            color: '#e0e0e0',
            fontSize: 14,
          }}
        />
        <label style={{ display: 'flex', alignItems: 'center', gap: 8, color: '#b0b0b0', fontSize: 13 }}>
          Sort
          <select
            value={sort}
            onChange={(e) => setSort(e.target.value as SortKey)}
            style={{
              padding: '8px 12px',
              borderRadius: 8,
              border: '1px solid #2a2d35',
              background: '#1a1d23',
              color: '#e0e0e0',
              fontSize: 13,
            }}
          >
            <option value="cpu">CPU %</option>
            <option value="pid">PID</option>
            <option value="name">Name</option>
          </select>
        </label>
      </div>

      {error && (
        <div style={{ ...card, marginBottom: 16, borderColor: '#7f1d1d', color: '#f87171' }}>{error}</div>
      )}

      <div style={{ ...card, padding: 0, overflow: 'auto' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', minWidth: 720 }}>
          <thead>
            <tr style={{ background: '#252830' }}>
              <th style={{ ...th, width: 36 }} />
              {headerBtn('PID', 'pid')}
              {headerBtn('COMM', 'name')}
              {headerBtn('CPU %', 'cpu')}
              <th style={{ ...th, cursor: 'default' }}>User %</th>
              <th style={{ ...th, cursor: 'default' }}>Sys %</th>
              <th style={{ ...th, cursor: 'default' }}>Threads</th>
              <th style={{ ...th, cursor: 'default' }}>RSS</th>
              <th style={{ ...th, cursor: 'default' }}>State</th>
            </tr>
          </thead>
          <tbody>
            {filteredSorted.map((p) => (
              <ProcessRow key={p.pid} proc={p} expanded={expanded.has(p.pid)} onToggle={() => toggle(p.pid)} />
            ))}
          </tbody>
        </table>
        {filteredSorted.length === 0 && (
          <div style={{ padding: 32, textAlign: 'center', color: '#666', fontSize: 14 }}>No processes match.</div>
        )}
      </div>
    </div>
  )
}

function ProcessRow({
  proc,
  expanded,
  onToggle,
}: {
  proc: ProcessInfo
  expanded: boolean
  onToggle: () => void
}) {
  const threads = proc.threads ?? []
  const expandable = proc.num_threads > 0 || threads.length > 0

  return (
    <>
      <tr
        onClick={() => expandable && onToggle()}
        style={{
          cursor: expandable ? 'pointer' : 'default',
          background: expanded ? '#22252d' : undefined,
        }}
      >
        <td style={td}>
          {expandable ? <span style={{ color: '#888' }}>{expanded ? '\u25BC' : '\u25B6'}</span> : ''}
        </td>
        <td style={{ ...td, fontFamily: 'monospace', color: '#93c5fd' }}>{proc.pid}</td>
        <td style={td}>{proc.comm}</td>
        <td style={td}>{proc.cpu_total_pct.toFixed(1)}</td>
        <td style={td}>{proc.cpu_user_pct.toFixed(1)}</td>
        <td style={td}>{proc.cpu_sys_pct.toFixed(1)}</td>
        <td style={td}>{proc.num_threads}</td>
        <td style={{ ...td, fontFamily: 'monospace', fontSize: 12 }}>{formatRssKb(proc.rss_kb)}</td>
        <td style={{ ...td, fontFamily: 'monospace' }}>{proc.state}</td>
      </tr>
      {expanded && threads.length === 0 && proc.num_threads > 0 && (
        <tr style={{ background: '#1f2229' }}>
          <td style={td} />
          <td colSpan={8} style={{ ...td, paddingLeft: 28, color: '#666', fontSize: 12 }}>
            <span style={{ color: '#555', marginRight: 6 }}>{'\u251C'}</span>
            Loading threads…
          </td>
        </tr>
      )}
      {expanded &&
        threads.map((t) => (
          <tr key={`${t.pid}-${t.tid}`} style={{ background: '#1f2229' }}>
            <td style={td} />
            <td style={{ ...td, paddingLeft: 28, fontFamily: 'monospace', color: '#7dd3fc' }}>
              <span style={{ color: '#555', marginRight: 6 }}>{'\u251C'}</span>
              {t.tid}
            </td>
            <td style={{ ...td, color: '#a8a8a8' }}>{t.comm}</td>
            <td style={td}>{t.cpu_total_pct.toFixed(1)}</td>
            <td style={td}>{t.cpu_user_pct.toFixed(1)}</td>
            <td style={td}>{t.cpu_sys_pct.toFixed(1)}</td>
            <td style={{ ...td, color: '#666' }}>—</td>
            <td style={{ ...td, color: '#666' }}>—</td>
            <td style={{ ...td, fontFamily: 'monospace' }}>{t.state}</td>
          </tr>
        ))}
    </>
  )
}
