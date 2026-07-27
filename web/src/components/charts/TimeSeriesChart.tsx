import { useEffect, useRef } from 'react';
import * as echarts from 'echarts/core';
import { LineChart } from 'echarts/charts';
import {
  GridComponent,
  TooltipComponent,
  DataZoomComponent,
  LegendComponent,
} from 'echarts/components';
import { CanvasRenderer } from 'echarts/renderers';

echarts.use([LineChart, GridComponent, TooltipComponent, DataZoomComponent, LegendComponent, CanvasRenderer]);

interface Series {
  name: string;
  data: { ts: number; value: number }[];
  color?: string;
}

interface TimeSeriesChartProps {
  series: Series[];
  height?: number;
  showDataZoom?: boolean;
  thresholds?: { value: number; color: string; label: string }[];
  className?: string;
}

export function TimeSeriesChart({
  series,
  height = 340,
  showDataZoom = true,
  thresholds = [],
  className,
}: TimeSeriesChartProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<echarts.ECharts>();

  useEffect(() => {
    if (!containerRef.current) return;
    const chart = echarts.init(containerRef.current, undefined, { renderer: 'canvas' });
    chartRef.current = chart;

    const handleResize = () => chart.resize();
    const ro = new ResizeObserver(handleResize);
    ro.observe(containerRef.current);

    return () => {
      ro.disconnect();
      chart.dispose();
    };
  }, []);

  useEffect(() => {
    const chart = chartRef.current;
    if (!chart) return;

    const markLines = thresholds.map((t) => ({
      yAxis: t.value,
      label: { formatter: t.label, position: 'end' as const, color: t.color, fontSize: 11 },
      lineStyle: { color: t.color, type: 'dashed' as const, width: 1 },
    }));

    const option: echarts.EChartsCoreOption = {
      backgroundColor: 'transparent',
      textStyle: { fontFamily: 'var(--font-sans)', color: '#a0a0b8' },
      legend: {
        show: series.length > 1,
        bottom: showDataZoom ? 40 : 0,
        textStyle: { color: '#a0a0b8', fontSize: 12 },
        itemWidth: 12,
        itemHeight: 3,
      },
      tooltip: {
        trigger: 'axis',
        backgroundColor: '#191922',
        borderColor: '#2a2a3a',
        textStyle: { color: '#eeeef5', fontSize: 12 },
        axisPointer: { type: 'cross', lineStyle: { color: '#2a2a3a' } },
      },
      grid: {
        top: 16,
        right: 16,
        bottom: showDataZoom ? 80 : (series.length > 1 ? 36 : 16),
        left: 0,
        containLabel: true,
      },
      xAxis: {
        type: 'time',
        axisLine: { lineStyle: { color: '#2a2a3a' } },
        axisTick: { show: false },
        axisLabel: { color: '#6b6b82', fontSize: 11 },
        splitLine: { show: false },
      },
      yAxis: {
        type: 'value',
        axisLine: { show: false },
        axisTick: { show: false },
        axisLabel: { color: '#6b6b82', fontSize: 11 },
        splitLine: { lineStyle: { color: '#1c1c28' } },
      },
      dataZoom: showDataZoom
        ? [{ type: 'slider', bottom: 8, height: 28, borderColor: '#2a2a3a', fillerColor: 'rgba(99,102,241,0.15)', handleStyle: { color: '#6366f1' } }]
        : [],
      series: series.map((s, i) => ({
        name: s.name,
        type: 'line',
        smooth: true,
        showSymbol: false,
        areaStyle: { opacity: 0.08 },
        lineStyle: { width: 1.5 },
        color: s.color ?? ['#6366f1', '#f97316', '#ef4444', '#22c55e', '#eab308'][i % 5],
        data: s.data.map((d) => [d.ts, d.value]),
        markLine: i === 0 && markLines.length > 0 ? { data: markLines, silent: true, symbol: 'none' } : undefined,
      })),
      animation: true,
      animationDuration: 500,
    };

    chart.setOption(option, true);
  }, [series, showDataZoom, thresholds]);

  return <div ref={containerRef} className={className} style={{ width: '100%', height }} />;
}
