import { useState, useMemo } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import { useGpuMonitor, useGpuProcesses } from '../hooks/useGpuData'
import type { GpuProcess } from '../hooks/useGpuData'
import EChart from '../components/charts/EChart'
import type { EChartsOption } from '../components/charts/EChart'
import FeatureHealthBadge from '../components/FeatureHealthBadge'
import { SummaryCard, Sparkline, EmptyChart } from '../components/shared'

const subTabs = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function GpuPage() {
  const [activeTab, setActiveTab] = useState('system')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, padding: '0 0 24px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '0 0 8px' }}>
        <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>GPU</h2>
        <FeatureHealthBadge featureName="gpu_monitor" />
      </div>
      <SubTabBar tabs={subTabs} active={activeTab} onChange={setActiveTab} />
      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const { data, summary } = useGpuMonitor(true)

  const utilOption = useMemo((): EChartsOption => {
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
        data: ['Compute', 'Memory'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 16, bottom: 32, left: 44 },
      xAxis: {
        type: 'category',
        data: times,
        boundaryGap: false,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 10 },
      },
      yAxis: {
        type: 'value',
        max: 100,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        splitLine: { lineStyle: { color: '#2a2d35' } },
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value}%' },
      },
      series: [
        {
          name: 'Compute',
          type: 'line',
          areaStyle: { opacity: 0.4 },
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.compute_pct),
          itemStyle: { color: '#22c55e' },
        },
        {
          name: 'Memory',
          type: 'line',
          areaStyle: { opacity: 0.3 },
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.memory_pct),
          itemStyle: { color: '#8b5cf6' },
        },
      ],
      animation: false,
    }
  }, [data])

  const thermalOption = useMemo((): EChartsOption => {
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
        data: ['Temperature', 'Power'],
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
      },
      grid: { top: 10, right: 60, bottom: 32, left: 44 },
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
          axisLabel: { color: '#888', fontSize: 10, formatter: '{value}°C' },
        },
        {
          type: 'value',
          axisLine: { lineStyle: { color: '#3a3d45' } },
          splitLine: { show: false },
          axisLabel: { color: '#f59e0b', fontSize: 10, formatter: '{value}W' },
        },
      ],
      series: [
        {
          name: 'Temperature',
          type: 'line',
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.temperature_c),
          itemStyle: { color: '#ef4444' },
          yAxisIndex: 0,
        },
        {
          name: 'Power',
          type: 'line',
          lineStyle: { width: 2 },
          symbol: 'none',
          data: data.map(d => d.power_w),
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
          <SummaryCard label="Compute" value={`${summary.computePct.toFixed(0)}%`}
            color={summary.computePct > 90 ? colors.danger : colors.success} />
          <SummaryCard label="VRAM" value={`${summary.memoryUsedMb.toFixed(0)}/${summary.memoryTotalMb.toFixed(0)} MB`} />
          <SummaryCard label="Temperature" value={`${summary.temperatureC.toFixed(0)}°C`}
            color={summary.temperatureC > 80 ? colors.danger : summary.temperatureC > 65 ? colors.warnText : colors.textSecondary} />
          <SummaryCard label="Power" value={`${summary.powerW.toFixed(0)} W`} />
        </div>
      )}

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>GPU Utilization</h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for GPU data..." />
          : <EChart option={utilOption} height={200} />
        }
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>Thermal & Power</h4>
        {data.length === 0
          ? <EmptyChart message="Waiting for thermal data..." />
          : <EChart option={thermalOption} height={160} />
        }
      </div>
    </div>
  )
}

function ProcessSubTab() {
  const { processes } = useGpuProcesses(true)

  return (
    <div style={{
      background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
      borderRadius: 8, padding: 16,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 13, color: colors.textSecondary }}>
        Process GPU Usage (Top by Compute)
      </h4>
      <GpuProcessTable processes={processes} />
    </div>
  )
}

function GpuProcessTable({ processes }: { processes: GpuProcess[] }) {
  if (processes.length === 0) {
    return <EmptyChart message="Waiting for GPU process data..." />
  }

  return (
    <div style={{ overflowX: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
            <th style={thStyle}>PID</th>
            <th style={thStyle}>Process</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>GPU %</th>
            <th style={{ ...thStyle, textAlign: 'right' }}>VRAM</th>
            <th style={{ ...thStyle, width: 100 }}>Trend</th>
          </tr>
        </thead>
        <tbody>
          {processes.slice(0, 15).map(p => (
            <tr key={p.pid} style={{ borderBottom: `1px solid ${colors.cardBorder}` }}>
              <td style={tdStyle}>{p.pid}</td>
              <td style={{ ...tdStyle, color: colors.accent }}>{p.comm}</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.gpu_pct.toFixed(1)}%</td>
              <td style={{ ...tdStyle, textAlign: 'right' }}>{p.memory_mb.toFixed(0)} MB</td>
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


const thStyle: React.CSSProperties = {
  padding: '8px 6px', textAlign: 'left', color: colors.textMuted, fontWeight: 500, fontSize: 11,
}

const tdStyle: React.CSSProperties = {
  padding: '8px 6px', color: colors.textSecondary, fontSize: 12,
}
