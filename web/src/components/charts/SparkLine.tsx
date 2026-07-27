import { useMemo } from 'react';

interface SparkLineProps {
  data: number[];
  width?: number;
  height?: number;
  color?: string;
  fillOpacity?: number;
  strokeWidth?: number;
  className?: string;
}

export function SparkLine({
  data,
  width,
  height = 36,
  color = 'var(--color-accent)',
  fillOpacity = 0.08,
  strokeWidth = 1.5,
  className,
}: SparkLineProps) {
  const viewBox = `0 0 ${data.length - 1} 1`;

  const { linePath, areaPath } = useMemo(() => {
    if (data.length < 2) return { linePath: '', areaPath: '' };

    const min = Math.min(...data);
    const max = Math.max(...data);
    const range = max - min || 1;

    const points = data.map((v, i) => ({
      x: i,
      y: 1 - (v - min) / range,
    }));

    const line = points.map((p, i) => `${i === 0 ? 'M' : 'L'}${p.x},${p.y}`).join(' ');
    const area = `${line} L${points.length - 1},1 L0,1 Z`;

    return { linePath: line, areaPath: area };
  }, [data]);

  if (data.length < 2) return null;

  return (
    <svg
      viewBox={viewBox}
      width={width}
      height={height}
      preserveAspectRatio="none"
      className={className}
      style={{ width: width ?? '100%', height, display: 'block' }}
    >
      <path d={areaPath} fill={color} opacity={fillOpacity} />
      <path
        d={linePath}
        fill="none"
        stroke={color}
        strokeWidth={strokeWidth / height}
        strokeLinecap="round"
        strokeLinejoin="round"
        vectorEffect="non-scaling-stroke"
      />
    </svg>
  );
}
