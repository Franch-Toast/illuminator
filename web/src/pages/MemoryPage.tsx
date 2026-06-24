import { useState, useMemo } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import { useMemoryUtilization, useMemoryProcesses } from '../hooks/useMemoryData'
import type { MemoryProcess } from '../hooks/useMemoryData'
import EChart from '../components/charts/EChart'
import type { EChartsOption } from '../components/charts/EChart'
import FeatureHealthBadge from '../components/FeatureHealthBadge'

const subTabs = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function MemoryPage() {
  const [activeTab, setActiveTab] = useState('system')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, padding: '0 0 24px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '0 0 8px' }}>
        <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>Memory</h2>
        <FeatureHealthBadge featureName="memory_utilization" />
      </div>
      <SubTabBar tabs={subTabs} active={activeTab} onChange={setActiveTab} />
      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const { data, summary } = useMemoryUtilization(true)

  const chartOption = useMemo((): EChartsOption => {
    const times = data.map(d => {
      const date = new Date(d.timestamp)
      return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`
    })

    return {
      tooltip: {
        trigger: 'axis',
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
        formatter: (params: unknown) => {
          const ps = params as Array<{ seriesName: string; value: number; color: string }>
          if (!Array.isArray(ps) || !ps.length) return ''
          let html = `<div style="font-size:11px">${ps[0].seriesName ? '' : ''}`
          for (const p of ps) {
            html += `<div><span style="color:${p.color}">●</span> ${p.seriesName}: ${p.value.toFixed(1)} MB</div>`
          }
          return html + '</div>'
        },
      },
      legend: {
        data: ['Used', 'Cached', 'Buffers', 'Free'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 16, bottom: 36, left: 52 },
      xAxis: {
        type: 'category',
        data: times,
        boundaryGap: false,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 10 },
      },
      yAxis: {
        type: 'value',
        axisLine: { lineStyle: { color: '#3a3d45' } },
        splitLine: { lineStyle: { color: '#2a2d35' } },
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value} MB' },
      },
      series: [
        {
          name: 'Used',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.6 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.used_mb),
          itemStyle: { color: '#ef4444' },
        },
        {
          name: 'Cached',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.5 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.cached_mb),
          itemStyle: { color: '#f59e0b' },
        },
        {
          name: 'Buffers',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.buffers_mb),
          itemStyle: { color: '#8b5cf6' },
        },
        {
          name: 'Free',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.3 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.free_mb),
          itemStyle: { color: '#4ade80' },
        },
      ],
      animation: false,
    }
  }, [data])

  const swapOption = useMemo((): EChartsOption => {
    const times = data.map(d => {
      const date = new Date(d.timestamp)
      return `${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`
    })

    return {
      tooltip: {
        trigger: 'axis',
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
      },
      grid: { top: 10, right: 16, bottom: 24, left: 52 },
      xAxis: {
        type: 'category',
        data: times,
        boundaryGap: false,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 10 },
      },
      yAxis: {
        type: 'value',
        axisLine: { lineStyle: { color: '#3a3d45' } },
        splitLine: { lineStyle: { color: '#2a2d35' } },
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value} MB' },
      },
      series: [
        {
          name: 'Swap Used',
          type: 'line',
          areaStyle: { opacity: 0.5, color: '#f97316' },
          lineStyle: { width: 1.5, color: '#f97316' },
          symbol: 'none',
          data: data.map(d => d.swap_used_mb),
        },
      ],
      animation: false,
    }
  }, [data])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      {/* Summary Cards */}
      {summary && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: 12 }}>
          <SummaryCard label="Total" value={`${summary.totalMb.toFixed(0)} MB`} />
          <SummaryCard label="Used" value={`${summary.usedPct.toFixed(1)}%`}
            color={summary.usedPct > 80 ? colors.danger : summary.usedPct > 60 ? colors.warnText : colors.success} />
          <SummaryCard label="Swap" value={`${summary.swapUsedPct.toFixed(1)}%`}
            color={summary.swapUsedPct > 50 ? colors.danger : colors.textSecondary} />
          <SummaryCard label="Page Faults" value={`${summary.pageFaults.toFixed(0)}/s`} />
        </div>
      )}

      {/* Memory Utilization Area Chart */}
      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Memory Utilization
        </h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for memory data..." />
          : <EChart option={chartOption} height={220} />
        }
      </div>

      {/* Swap Usage */}
      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Swap Usage
        </h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for swap data..." />
          : <EChart option={swapOption} height={140} />
        }
      </div>
    </div>
  )
}

function ProcessSubTab() {
  const { processes } = useMemoryProcesses(true)
  const [selectedPid, setSelectedPid] = useState<number | null>(null)
  const [selectedComm, setSelectedComm] = useState('')

  const handleSelect = (pid: number) => {
    const proc = processes.find(p => p.pid === pid)
    if (proc) {
      setSelectedPid(pid)
      setSelectedComm(proc.comm)
    }
  }

  if (selectedPid) {
    return <MemoryProcessDetail pid={selectedPid} comm={selectedComm} onBack={() => setSelectedPid(null)} />
  }

  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: 16,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 13, color: colors.textSecondary }}>
        Process Memory (Top by RSS)
      </h4>
      <MemoryProcessTable processes={processes} onSelect={handleSelect} />
    </div>
  )
}

function MemoryProcessTable({ processes, onSelect }: { processes: MemoryProcess[]; onSelect: (pid: number) => void }) {
  if (processes.length === 0) {
    return <EmptyChart message="Waiting for process memory data..." />
  }

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
            <th style={thStyle}>PID</th>
            <th style={thStyle}>Process</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>RSS</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>VMS</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>Shared</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>Swap</th>
            <th style={{ ...thStyle, width: 100 }}>Trend</th>
          </tr>
        </thead>
        <tbody>
          {processes.slice(0, 20).map(p => (
            <tr key={p.pid}
              onClick={() => onSelect(p.pid)}
              style={{ borderBottom: `1px solid ${colors.cardBorder}`, cursor: 'pointer' }}
              onMouseEnter={e => (e.currentTarget.style.background = 'rgba(255,255,255,0.03)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
            >
              <td style={tdStyle}>{p.pid}</td>
              <td style={{ ...tdStyle, color: colors.accent }}>{p.comm}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{formatMb(p.rss_mb)}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{formatMb(p.vms_mb)}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{formatMb(p.shared_mb)}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{formatMb(p.swap_mb)}</td>
              <td style={tdStyle}>
                <Sparkline data={p.history} color="#3b82f6" />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function MemoryProcessDetail({ pid, comm, onBack }: { pid: number; comm: string; onBack: () => void }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <button onClick={onBack} style={{
          background: 'transparent', border: `1px solid ${colors.cardBorder}`,
          borderRadius: 4, padding: '4px 10px', color: colors.textSecondary,
          cursor: 'pointer', fontSize: 12,
        }}>
          ← Back
        </button>
        <h3 style={{ margin: 0, fontSize: 15, color: colors.textPrimary }}>
          {comm} <span style={{ color: colors.textMuted, fontSize: 12 }}>(PID: {pid})</span>
        </h3>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 24, textAlign: 'center',
      }}>
        <p style={{ color: colors.textMuted, fontSize: 13 }}>
          Heap profiling (jemalloc/tcmalloc) for this process will be available here.
        </p>
        <p style={{ color: colors.textMuted, fontSize: 11, marginTop: 8 }}>
          Features: allocation tracking, leak detection, heap profile diff
        </p>
      </div>
    </div>
  )
}

function SummaryCard({ label, value, color }: { label: string; value: string; color?: string }) {
  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: '12px 16px',
    }}>
      <div style={{ fontSize: 10, color: colors.textMuted, marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 18, fontWeight: 600, fontVariantNumeric: 'tabular-nums', color: color ?? colors.textPrimary }}>
        {value}
      </div>
    </div>
  )
}

function Sparkline({ data, color }: { data: number[]; color: string }) {
  if (data.length < 2) return null
  const max = Math.max(...data, 1)
  const h = 20
  const w = 100
  const step = w / (data.length - 1)
  const points = data.map((v, i) => `${i * step},${h - (v / max) * h}`).join(' ')

  return (
    <svg width={w} height={h} style={{ display: 'block' }}>
      <polyline fill="none" stroke={color} strokeWidth="1.5" points={points} />
    </svg>
  )
}

function EmptyChart({ message }: { message: string }) {
  return (
    <div style={{
      height: 100, display: 'flex', alignItems: 'center', justifyContent: 'center',
      color: colors.textMuted, fontSize: 12,
    }}>
      {message}
    </div>
  )
}

function formatMb(mb: number): string {
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`
  return `${mb.toFixed(1)} MB`
}

const thStyle: React.CSSProperties = {
  padding: '8px 6px', textAlign: 'left', color: colors.textMuted, fontWeight: 500, fontSize: 11,
}

const tdStyle: React.CSSProperties = {
  padding: '8px 6px', color: colors.textSecondary, fontSize: 12,
}
