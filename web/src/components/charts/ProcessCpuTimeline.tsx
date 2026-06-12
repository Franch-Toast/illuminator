import { useMemo, useEffect, useRef } from 'react'
import EChart, { echarts } from './EChart'
import type { EChartsOption } from './EChart'
import type { TimeSelection } from './ProfileSnapshot'
import { useTimeStore } from '../../stores/useTimeStore'

interface TimelinePoint {
  timestamp: number
  cpu_user_pct: number
  cpu_sys_pct: number
}

interface ProcessCpuTimelineProps {
  data: TimelinePoint[]
  selectedTimestamp: number | null
  onTimeSelect?: (selection: TimeSelection | null) => void
  width?: number | string
  height?: number
}

export default function ProcessCpuTimeline({
  data,
  onTimeSelect,
  width = '100%',
  height = 200,
}: ProcessCpuTimelineProps) {
  const mode = useTimeStore(s => s.mode)
  const chartRef = useRef<echarts.ECharts | null>(null)
  const dataRef = useRef(data)
  const modeRef = useRef(mode)
  const onTimeSelectRef = useRef(onTimeSelect)

  dataRef.current = data
  modeRef.current = mode
  onTimeSelectRef.current = onTimeSelect

  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return

    chart.off('click')
    chart.off('brushEnd')

    chart.on('click', (params: unknown) => {
      const p = params as { dataIndex?: number }
      if (modeRef.current !== 'paused' || !onTimeSelectRef.current || p.dataIndex === undefined) return
      const currentData = dataRef.current
      const point = currentData[p.dataIndex]
      if (!point) return
      onTimeSelectRef.current({
        type: 'point',
        start: point.timestamp - 500,
        end: point.timestamp + 500,
      })
    })

    chart.on('brushEnd', (params: unknown) => {
      const p = params as { areas?: Array<{ coordRange?: number[] }> }
      if (!onTimeSelectRef.current || !p.areas?.length) return
      const area = p.areas[0]
      if (!area?.coordRange) return
      const [startIdx, endIdx] = area.coordRange
      const currentData = dataRef.current
      if (startIdx === undefined || endIdx === undefined) return
      const startTs = currentData[Math.max(0, Math.round(startIdx))]?.timestamp
      const endTs = currentData[Math.min(currentData.length - 1, Math.round(endIdx))]?.timestamp
      if (startTs && endTs) {
        onTimeSelectRef.current({ type: 'range', start: Math.min(startTs, endTs), end: Math.max(startTs, endTs) })
      }
    })
  }, [])

  const handleInit = (chart: echarts.ECharts) => {
    chartRef.current = chart
  }

  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return
    if (mode === 'paused') {
      try {
        chart.dispatchAction({ type: 'takeGlobalCursor', key: 'brush', brushOption: { brushType: 'lineX' } })
      } catch { /* brush not available */ }
    } else {
      try {
        chart.dispatchAction({ type: 'takeGlobalCursor', key: 'brush', brushOption: { brushType: false } })
      } catch { /* ignore */ }
      if (onTimeSelect) onTimeSelect(null)
    }
  }, [mode])

  const option = useMemo((): EChartsOption => {
    const times = data.map(d => {
      const date = new Date(d.timestamp)
      return `${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`
    })

    const base: EChartsOption = {
      toolbox: { show: false, feature: { brush: { type: ['lineX', 'clear'] } } },
      brush: {
        toolbox: ['lineX', 'clear'],
        brushStyle: { borderWidth: 1, color: 'rgba(96,165,250,0.15)', borderColor: '#60a5fa' },
        xAxisIndex: 0,
        throttleType: 'debounce',
        throttleDelay: 300,
      },
      tooltip: {
        trigger: 'axis',
        backgroundColor: '#1a1d23',
        borderColor: '#2a2d35',
        textStyle: { color: '#e0e0e0', fontSize: 12 },
      },
      legend: {
        data: ['User', 'System'],
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
        min: 0,
        axisLine: { lineStyle: { color: '#3a3d45' } },
        splitLine: { lineStyle: { color: '#2a2d35' } },
        axisLabel: { color: '#888', fontSize: 10, formatter: '{value}%' },
      },
      series: [
        {
          name: 'User',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.6, color: '#3b82f6' },
          lineStyle: { width: 2, color: '#3b82f6' },
          symbol: 'none',
          data: data.map(d => Number(d.cpu_user_pct.toFixed(1))),
          itemStyle: { color: '#3b82f6' },
        },
        {
          name: 'System',
          type: 'line',
          stack: 'total',
          areaStyle: { opacity: 0.6, color: '#ef4444' },
          lineStyle: { width: 2, color: '#ef4444' },
          symbol: 'none',
          data: data.map(d => Number(d.cpu_sys_pct.toFixed(1))),
          itemStyle: { color: '#ef4444' },
        },
      ],
      animation: false,
    }

    return base
  }, [data])

  return (
    <div>
      <EChart option={option} width={width} height={height} onInit={handleInit} />
      {mode === 'paused' && (
        <div style={{ fontSize: 10, color: '#6b7280', textAlign: 'center', marginTop: 4 }}>
          Click a time point or drag to select a range — flame graph will aggregate samples in that window
        </div>
      )}
      {mode === 'live' && data.length > 0 && (
        <div style={{ fontSize: 10, color: '#6b7280', textAlign: 'center', marginTop: 4 }}>
          Press Space to pause and select time range for flame graph
        </div>
      )}
    </div>
  )
}
