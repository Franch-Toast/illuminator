import { useMemo } from 'react'
import EChart from './EChart'
import type { EChartsOption } from './EChart'

export interface CoreDataPoint {
  timestamp: number
  cores: { name: string; busy_pct: number }[]
}

interface CoreHeatmapProps {
  data: CoreDataPoint[]
  width?: number | string
  height?: number
}

export default function CoreHeatmap({ data, width = '100%', height }: CoreHeatmapProps) {
  const option = useMemo((): EChartsOption => {
    if (data.length === 0) return {}

    const coreNames = data[0].cores.map(c => c.name)

    const heatmapData: [number, number, number][] = []
    const times: string[] = []

    for (let xi = 0; xi < data.length; xi++) {
      const d = data[xi]
      const date = new Date(d.timestamp)
      times.push(`${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`)
      for (let yi = 0; yi < d.cores.length; yi++) {
        heatmapData.push([xi, yi, Math.round(d.cores[yi].busy_pct)])
      }
    }

    return {
      tooltip: {
        position: 'top',
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
        formatter: (params: unknown) => {
          const p = params as { value: [number, number, number] }
          return `${coreNames[p.value[1]]}: ${p.value[2]}%`
        },
      },
      grid: { top: 4, right: 16, bottom: 24, left: 60 },
      xAxis: {
        type: 'category',
        data: times,
        splitArea: { show: false },
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#888', fontSize: 9, interval: 'auto' },
      },
      yAxis: {
        type: 'category',
        data: coreNames,
        splitArea: { show: false },
        axisLine: { lineStyle: { color: '#3a3d45' } },
        axisLabel: { color: '#b0b0b0', fontSize: 10 },
      },
      visualMap: {
        min: 0,
        max: 100,
        show: false,
        inRange: {
          color: ['#1a1d23', '#1d4ed8', '#dc2626', '#fbbf24'],
        },
      },
      series: [{
        type: 'heatmap',
        data: heatmapData,
        emphasis: { itemStyle: { shadowBlur: 6, shadowColor: 'rgba(0,0,0,0.4)' } },
      }],
      animation: false,
    } as EChartsOption
  }, [data])

  const computedHeight = height ?? Math.max(160, (data[0]?.cores.length ?? 4) * 18 + 60)

  return <EChart option={option} width={width} height={computedHeight} />
}
