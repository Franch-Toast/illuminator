import { useEffect, useRef } from 'react'
import * as echarts from 'echarts/core'
import type { EChartsCoreOption } from 'echarts/core'
import { LineChart, HeatmapChart, BarChart } from 'echarts/charts'
import {
  GridComponent,
  TooltipComponent,
  DataZoomComponent,
  LegendComponent,
  VisualMapComponent,
  BrushComponent,
  ToolboxComponent,
} from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'

echarts.use([
  LineChart, HeatmapChart, BarChart,
  GridComponent, TooltipComponent, DataZoomComponent,
  LegendComponent, VisualMapComponent, BrushComponent,
  ToolboxComponent, CanvasRenderer,
])

const darkTheme = {
  backgroundColor: 'transparent',
  textStyle: { color: '#b0b0b0' },
  legend: { textStyle: { color: '#b0b0b0' } },
  categoryAxis: {
    axisLine: { lineStyle: { color: '#3a3d45' } },
    splitLine: { lineStyle: { color: '#2a2d35' } },
    axisLabel: { color: '#888' },
  },
  valueAxis: {
    axisLine: { lineStyle: { color: '#3a3d45' } },
    splitLine: { lineStyle: { color: '#2a2d35' } },
    axisLabel: { color: '#888' },
  },
}

echarts.registerTheme('illuminator', darkTheme)

interface EChartProps {
  option: EChartsCoreOption
  width?: number | string
  height?: number | string
  group?: string
  onInit?: (chart: echarts.ECharts) => void
}

export default function EChart({ option, width = '100%', height = 260, group, onInit }: EChartProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<echarts.ECharts | null>(null)
  const observerRef = useRef<IntersectionObserver | null>(null)
  const visibleRef = useRef(true)

  useEffect(() => {
    if (!containerRef.current) return

    const chart = echarts.init(containerRef.current, 'illuminator')
    chartRef.current = chart

    if (group) {
      chart.group = group
      echarts.connect(group)
    }

    onInit?.(chart)

    const resizeObserver = new ResizeObserver(() => {
      if (visibleRef.current) chart.resize()
    })
    resizeObserver.observe(containerRef.current)

    observerRef.current = new IntersectionObserver(
      ([entry]) => { visibleRef.current = entry.isIntersecting },
      { threshold: 0.1 }
    )
    observerRef.current.observe(containerRef.current)

    return () => {
      resizeObserver.disconnect()
      observerRef.current?.disconnect()
      chart.dispose()
      chartRef.current = null
    }
  }, [])

  useEffect(() => {
    if (!chartRef.current) return
    chartRef.current.setOption(option, { notMerge: false, lazyUpdate: !visibleRef.current })
  }, [option])

  return <div ref={containerRef} style={{ width, height }} />
}

export type EChartsOption = EChartsCoreOption
export { echarts }
