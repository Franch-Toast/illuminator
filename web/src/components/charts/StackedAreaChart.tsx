import { useMemo, useCallback } from 'react'
import EChart from './EChart'
import type { EChartsOption } from './EChart'
import { downsample } from '../../utils/downsample'

export interface CpuDataPoint {
  timestamp: number
  user_pct: number
  system_pct: number
  irq_pct: number
  softirq_pct: number
  iowait_pct: number
  steal_pct: number
  idle_pct: number
}

interface StackedAreaChartProps {
  data: CpuDataPoint[]
  width?: number | string
  height?: number
  group?: string
}

const LAYERS = [
  { key: 'user_pct' as const, color: '#3b82f6', label: 'User' },
  { key: 'system_pct' as const, color: '#ef4444', label: 'System' },
  { key: 'iowait_pct' as const, color: '#eab308', label: 'IOWait' },
  { key: 'irq_pct' as const, color: '#f97316', label: 'IRQ' },
  { key: 'softirq_pct' as const, color: '#ec4899', label: 'SoftIRQ' },
  { key: 'steal_pct' as const, color: '#9333ea', label: 'Steal' },
]

const DOWNSAMPLE_THRESHOLD = 1000

function formatTime(ts: number): string {
  const date = new Date(ts)
  return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`
}

export default function StackedAreaChart({ data, width = '100%', height = 240, group }: StackedAreaChartProps) {
  const displayData = useMemo(() => {
    if (data.length <= DOWNSAMPLE_THRESHOLD) return data
    const indexed = data.map((d, i) => ({ x: i, y: d.user_pct + d.system_pct + d.iowait_pct + d.irq_pct + d.softirq_pct + d.steal_pct }))
    const sampled = downsample(indexed, { threshold: DOWNSAMPLE_THRESHOLD, algorithm: 'interval' })
    return sampled.map(p => data[p.x])
  }, [data])

  const times = useMemo(
    () => displayData.map(d => formatTime(d.timestamp)),
    [displayData]
  )

  const handleFormatter = useCallback((params: unknown) => {
    const items = params as Array<{ seriesName: string; value: number; color: string; dataIndex: number }>
    if (!items?.length) return ''
    const idx = items[0].dataIndex ?? 0
    const time = times[idx] ?? ''
    let html = `<div style="font-size:11px;color:#888">${time}</div>`
    for (const item of items) {
      if (item.value > 0.1) {
        html += `<div><span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${item.color};margin-right:4px"></span>${item.seriesName}: ${item.value.toFixed(1)}%</div>`
      }
    }
    return html
  }, [times])

  const option = useMemo((): EChartsOption => {
    return {
      tooltip: {
        trigger: 'axis',
        axisPointer: { type: 'cross' },
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
        formatter: handleFormatter,
      },
      legend: {
        data: LAYERS.map(l => l.label),
        bottom: 0,
        textStyle: { color: '#b0b0b0', fontSize: 11 },
        itemWidth: 12,
        itemHeight: 8,
      },
      grid: { top: 10, right: 16, bottom: 36, left: 44 },
      xAxis: {
        type: 'category',
        data: times,
        boundaryGap: false,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 10, interval: 'auto' },
      },
      yAxis: {
        type: 'value',
        max: 100,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        splitLine: { lineStyle: { color: '#2a2d35' } },
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value}%' },
      },
      series: LAYERS.map(layer => ({
        name: layer.label,
        type: 'line' as const,
        stack: 'cpu',
        areaStyle: { opacity: 0.7 },
        lineStyle: { width: 0 },
        symbol: 'none',
        data: displayData.map(d => d[layer.key] || 0),
        itemStyle: { color: layer.color },
      })),
      animation: false,
    }
  }, [times, displayData, handleFormatter])

  return <EChart option={option} width={width} height={height} group={group} />
}
