import { useState, useEffect, useRef } from 'react'
import { colors } from '../../styles/theme'
import type { ReplayEngine } from '../../services/replayEngine'
import type { DataBatch } from '../../services/dataSource'
import EChart from '../../components/charts/EChart'
import type { EChartsOption } from 'echarts'

interface MemoryPoint {
  timestamp: number
  used_mb: number
  cached_mb: number
  free_mb: number
  total_mb: number
  swap_used_mb: number
}

export default function ReplayMemoryView({ engine }: { engine: ReplayEngine }) {
  const [data, setData] = useState<MemoryPoint[]>([])
  const bufferRef = useRef<MemoryPoint[]>([])

  useEffect(() => {
    const unsub = engine.subscribe('memory_utilization', (batch: DataBatch) => {
      const raw = batch.data as { records?: Array<{ labels?: Record<string, string>; fields?: Record<string, number> }> }
      if (!raw?.records) return

      for (const rec of raw.records) {
        if (rec.labels?.type !== 'memory_total') continue
        const f = rec.fields ?? {}
        bufferRef.current.push({
          timestamp: batch.timestamp,
          used_mb: (f.used_bytes ?? 0) / (1024 * 1024),
          cached_mb: (f.cached_bytes ?? 0) / (1024 * 1024),
          free_mb: (f.free_bytes ?? 0) / (1024 * 1024),
          total_mb: (f.total_bytes ?? 0) / (1024 * 1024),
          swap_used_mb: (f.swap_used_bytes ?? 0) / (1024 * 1024),
        })
        if (bufferRef.current.length > 120) bufferRef.current.shift()
      }
      setData([...bufferRef.current])
    })
    return unsub
  }, [engine])

  const option: EChartsOption = {
    tooltip: { trigger: 'axis', backgroundColor: '#1a1d23', borderColor: '#2a2d35', textStyle: { color: '#e0e0e0', fontSize: 11 } },
    legend: { bottom: 0, textStyle: { color: '#b0b0b0', fontSize: 11 } },
    grid: { top: 10, right: 16, bottom: 36, left: 50 },
    xAxis: {
      type: 'category',
      data: data.map(d => {
        const dt = new Date(d.timestamp)
        return `${dt.getMinutes().toString().padStart(2, '0')}:${dt.getSeconds().toString().padStart(2, '0')}`
      }),
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
      { name: 'Used', type: 'line', stack: 'mem', areaStyle: { opacity: 0.6 }, symbol: 'none', data: data.map(d => d.used_mb.toFixed(0)), lineStyle: { width: 1.5 }, itemStyle: { color: '#ef4444' } },
      { name: 'Cached', type: 'line', stack: 'mem', areaStyle: { opacity: 0.4 }, symbol: 'none', data: data.map(d => d.cached_mb.toFixed(0)), lineStyle: { width: 1.5 }, itemStyle: { color: '#f59e0b' } },
      { name: 'Free', type: 'line', stack: 'mem', areaStyle: { opacity: 0.3 }, symbol: 'none', data: data.map(d => d.free_mb.toFixed(0)), lineStyle: { width: 1.5 }, itemStyle: { color: '#10b981' } },
    ],
    animation: false,
  }

  return (
    <div style={{
      background: colors.cardBg, borderRadius: 8, padding: 16,
      border: `1px solid ${colors.cardBorder}`,
    }}>
      <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
        Memory Utilization (Replay)
      </h4>
      {data.length === 0 ? (
        <div style={{ padding: 40, textAlign: 'center', color: colors.textMuted, fontSize: 13 }}>
          Press play to start replaying memory data
        </div>
      ) : (
        <EChart option={option} height={250} />
      )}
    </div>
  )
}
