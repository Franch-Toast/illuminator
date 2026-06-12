import { useMemo } from 'react'
import EChart from './EChart'
import type { EChartsOption } from './EChart'

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

export default function StackedAreaChart({ data, width = '100%', height = 240, group }: StackedAreaChartProps) {
  const option = useMemo((): EChartsOption => {
    const times = data.map(d => {
      const date = new Date(d.timestamp)
      return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`
    })

    return {
      tooltip: {
        trigger: 'axis',
        axisPointer: { type: 'cross' },
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
        formatter: (params: unknown) => {
          const items = params as Array<{ seriesName: string; value: number; color: string }>
          if (!items?.length) return ''
          let html = `<div style="font-size:11px;color:#888">${items[0].seriesName ? times[0] : ''}</div>`
          for (const item of items) {
            if (item.value > 0.1) {
              html += `<div><span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${item.color};margin-right:4px"></span>${item.seriesName}: ${item.value.toFixed(1)}%</div>`
            }
          }
          return html
        },
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
        data: data.map(d => d[layer.key] || 0),
        itemStyle: { color: layer.color },
      })),
      animation: false,
    }
  }, [data])

  return <EChart option={option} width={width} height={height} group={group} />
}
