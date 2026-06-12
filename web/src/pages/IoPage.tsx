import { useState, useMemo } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import { usePageActivation, useFeaturesByCategory } from '../hooks/useDataSource'
import { useIoMonitor, useIoProcesses } from '../hooks/useIoData'
import type { IoProcess } from '../hooks/useIoData'
import EChart from '../components/charts/EChart'
import type { EChartsOption } from '../components/charts/EChart'

const subTabs = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function IoPage() {
  const [activeTab, setActiveTab] = useState('system')
  usePageActivation('io', [{ name: 'io_monitor', tier: 2 }])
  useFeaturesByCategory('io')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, padding: '0 0 24px' }}>
      <SubTabBar tabs={subTabs} active={activeTab} onChange={setActiveTab} />
      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const { data, summary } = useIoMonitor(true)

  const iopsOption = useMemo((): EChartsOption => {
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
      legend: {
        data: ['Read IOPS', 'Write IOPS'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 16, bottom: 32, left: 52 },
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
        axisLabel: { color: '#888', fontSize: 10 },
      },
      series: [
        {
          name: 'Read IOPS',
          type: 'line',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.read_iops),
          itemStyle: { color: '#3b82f6' },
        },
        {
          name: 'Write IOPS',
          type: 'line',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.write_iops),
          itemStyle: { color: '#ef4444' },
        },
      ],
      animation: false,
    }
  }, [data])

  const latencyOption = useMemo((): EChartsOption => {
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
      legend: {
        data: ['Avg Latency', 'P99 Latency'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 16, bottom: 32, left: 52 },
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
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value} μs' },
      },
      series: [
        {
          name: 'Avg Latency',
          type: 'line',
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.avg_latency_us),
          itemStyle: { color: '#8b5cf6' },
        },
        {
          name: 'P99 Latency',
          type: 'line',
          lineStyle: { width: 2, type: 'dashed' },
          symbol: 'none',
          data: data.map(d => d.p99_latency_us),
          itemStyle: { color: '#f59e0b' },
        },
      ],
      animation: false,
    }
  }, [data])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      {summary && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: 12 }}>
          <SummaryCard label="Total IOPS" value={`${summary.totalIops.toFixed(0)}`} />
          <SummaryCard label="Read Throughput" value={`${summary.readThroughput.toFixed(1)} MB/s`} />
          <SummaryCard label="Write Throughput" value={`${summary.writeThroughput.toFixed(1)} MB/s`} />
          <SummaryCard label="Avg Latency" value={`${summary.avgLatencyUs.toFixed(0)} μs`}
            color={summary.avgLatencyUs > 10000 ? colors.danger : colors.textSecondary} />
        </div>
      )}

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>IOPS</h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for IO data..." />
          : <EChart option={iopsOption} height={200} />
        }
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>IO Latency</h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for latency data..." />
          : <EChart option={latencyOption} height={180} />
        }
      </div>
    </div>
  )
}

function ProcessSubTab() {
  const { processes } = useIoProcesses(true)

  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: 16,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 13, color: colors.textSecondary }}>
        Process IO (Top by IOPS)
      </h4>
      <IoProcessTable processes={processes} />
    </div>
  )
}

function IoProcessTable({ processes }: { processes: IoProcess[] }) {
  if (processes.length === 0) {
    return <EmptyChart message="Waiting for process IO data..." />
  }

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
            <th style={thStyle}>PID</th>
            <th style={thStyle}>Process</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>Read</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>Write</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>IOPS</th>
            <th style={{ ...thStyle, width: 100 }}>Trend</th>
          </tr>
        </thead>
        <tbody>
          {processes.slice(0, 20).map(p => (
            <tr key={p.pid} style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              <td style={tdStyle}>{p.pid}</td>
              <td style={{ ...tdStyle, color: colors.accent }}>{p.comm}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.read_mb.toFixed(1)} MB/s</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.write_mb.toFixed(1)} MB/s</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.iops}</td>
              <td style={tdStyle}>
                <Sparkline data={p.history} color="#8b5cf6" />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
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

const thStyle: React.CSSProperties = {
  padding: '8px 6px', textAlign: 'left', color: colors.textMuted, fontWeight: 500, fontSize: 11,
}

const tdStyle: React.CSSProperties = {
  padding: '8px 6px', color: colors.textSecondary, fontSize: 12,
}
