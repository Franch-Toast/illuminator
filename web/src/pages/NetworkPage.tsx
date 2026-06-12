import { useState, useMemo } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import { usePageActivation, useFeaturesByCategory } from '../hooks/useDataSource'
import { useNetworkMonitor, useNetworkProcesses, formatBytes } from '../hooks/useNetworkData'
import type { NetworkProcess } from '../hooks/useNetworkData'
import EChart from '../components/charts/EChart'
import type { EChartsOption } from '../components/charts/EChart'

const subTabs = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function NetworkPage() {
  const [activeTab, setActiveTab] = useState('system')
  usePageActivation('network', [{ name: 'net_tracer', tier: 2 }])
  useFeaturesByCategory('network')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, padding: '0 0 24px' }}>
      <SubTabBar tabs={subTabs} active={activeTab} onChange={setActiveTab} />
      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const { data, summary } = useNetworkMonitor(true)

  const trafficOption = useMemo((): EChartsOption => {
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
        data: ['RX', 'TX'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 16, bottom: 32, left: 60 },
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
        axisLabel: {
          color: '#888', fontSize: 10,
          formatter: (v: number) => {
            if (v >= 1024 * 1024) return `${(v / (1024 * 1024)).toFixed(0)} MB/s`
            if (v >= 1024) return `${(v / 1024).toFixed(0)} KB/s`
            return `${v} B/s`
          },
        },
      },
      series: [
        {
          name: 'RX',
          type: 'line',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.rx_bytes_per_sec),
          itemStyle: { color: '#22c55e' },
        },
        {
          name: 'TX',
          type: 'line',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 1.5 },
          symbol: 'none',
          data: data.map(d => d.tx_bytes_per_sec),
          itemStyle: { color: '#3b82f6' },
        },
      ],
      animation: false,
    }
  }, [data])

  const connOption = useMemo((): EChartsOption => {
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
        data: ['Connections', 'Retransmits/s'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 60, bottom: 32, left: 52 },
      xAxis: {
        type: 'category',
        data: times,
        boundaryGap: false,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 10 },
      },
      yAxis: [
        {
          type: 'value',
          axisLine: { lineStyle: { color: '#3a3d45' } },
          splitLine: { lineStyle: { color: '#2a2d35' } },
          axisLabel: { color: '#888', fontSize: 10 },
        },
        {
          type: 'value',
          axisLine: { lineStyle: { color: '#3a3d45' } },
          splitLine: { show: false },
          axisLabel: { color: '#f59e0b', fontSize: 10 },
        },
      ],
      series: [
        {
          name: 'Connections',
          type: 'line',
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.tcp_connections),
          itemStyle: { color: '#60a5fa' },
          yAxisIndex: 0,
        },
        {
          name: 'Retransmits/s',
          type: 'bar',
          data: data.map(d => d.retransmits_per_sec),
          itemStyle: { color: '#f59e0b' },
          yAxisIndex: 1,
        },
      ],
      animation: false,
    }
  }, [data])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      {summary && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: 12 }}>
          <SummaryCard label="RX Rate" value={formatBytes(summary.rxRate)} />
          <SummaryCard label="TX Rate" value={formatBytes(summary.txRate)} />
          <SummaryCard label="TCP Connections" value={`${summary.connections}`} />
          <SummaryCard label="Retransmits" value={`${summary.retransmits}/s`}
            color={summary.retransmits > 10 ? colors.danger : colors.textSecondary} />
        </div>
      )}

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>Network Traffic</h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for network data..." />
          : <EChart option={trafficOption} height={200} />
        }
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Connections & Retransmits
        </h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for connection data..." />
          : <EChart option={connOption} height={180} />
        }
      </div>
    </div>
  )
}

function ProcessSubTab() {
  const { processes } = useNetworkProcesses(true)

  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: 16,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 13, color: colors.textSecondary }}>
        Process Network (Top by Traffic)
      </h4>
      <NetworkProcessTable processes={processes} />
    </div>
  )
}

function NetworkProcessTable({ processes }: { processes: NetworkProcess[] }) {
  if (processes.length === 0) {
    return <EmptyChart message="Waiting for process network data..." />
  }

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
            <th style={thStyle}>PID</th>
            <th style={thStyle}>Process</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>RX</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>TX</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>Conns</th>
            <th style={{ ...thStyle, width: 100 }}>Trend</th>
          </tr>
        </thead>
        <tbody>
          {processes.slice(0, 20).map(p => (
            <tr key={p.pid} style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              <td style={tdStyle}>{p.pid}</td>
              <td style={{ ...tdStyle, color: colors.accent }}>{p.comm}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.rx_mb.toFixed(2)} MB/s</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.tx_mb.toFixed(2)} MB/s</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.connections}</td>
              <td style={tdStyle}>
                <Sparkline data={p.history} color="#22c55e" />
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
  const max = Math.max(...data, 0.01)
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
