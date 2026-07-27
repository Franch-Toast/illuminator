import { useRef, useEffect, useCallback, useState } from 'react';
import type { FlameFrame } from '../../mock/data';

interface FlameGraphCanvasProps {
  data: FlameFrame;
  height?: number;
  className?: string;
}

interface FlatFrame {
  name: string;
  depth: number;
  x: number;
  w: number;
  self: number;
  total: number;
}

function flatten(frame: FlameFrame, depth: number, x: number, totalRoot: number): FlatFrame[] {
  const result: FlatFrame[] = [];
  const w = frame.value / totalRoot;
  result.push({ name: frame.name, depth, x, w, self: frame.selfValue, total: frame.value });

  let childX = x;
  for (const child of frame.children) {
    result.push(...flatten(child, depth + 1, childX, totalRoot));
    childX += child.value / totalRoot;
  }
  return result;
}

function hashColor(name: string): string {
  let hash = 0;
  for (let i = 0; i < name.length; i++) {
    hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0;
  }
  const hue = Math.abs(hash) % 360;
  return `hsl(${hue}, 55%, 55%)`;
}

const ROW_HEIGHT = 22;

export function FlameGraphCanvas({ data, height: propHeight, className }: FlameGraphCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [tooltip, setTooltip] = useState<{ x: number; y: number; frame: FlatFrame } | null>(null);
  const framesRef = useRef<FlatFrame[]>([]);
  const maxDepthRef = useRef(0);

  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);

    const W = rect.width;
    const H = rect.height;
    ctx.clearRect(0, 0, W, H);

    const frames = flatten(data, 0, 0, data.value);
    framesRef.current = frames;

    let maxD = 0;
    for (const f of frames) if (f.depth > maxD) maxD = f.depth;
    maxDepthRef.current = maxD;

    for (const f of frames) {
      const px = f.x * W;
      const py = f.depth * ROW_HEIGHT;
      const pw = f.w * W;

      if (pw < 0.5) continue;

      ctx.fillStyle = hashColor(f.name);
      ctx.fillRect(px + 0.5, py + 0.5, Math.max(pw - 1, 1), ROW_HEIGHT - 1);

      if (pw > 40) {
        ctx.fillStyle = '#eeeef5';
        ctx.font = '11px "JetBrains Mono", monospace';
        ctx.textBaseline = 'middle';
        const maxTextW = pw - 6;
        let text = f.name;
        const measured = ctx.measureText(text).width;
        if (measured > maxTextW) {
          const ratio = maxTextW / measured;
          text = text.slice(0, Math.max(1, Math.floor(text.length * ratio) - 2)) + '…';
        }
        ctx.fillText(text, px + 3, py + ROW_HEIGHT / 2);
      }
    }
  }, [data]);

  useEffect(() => {
    draw();
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ro = new ResizeObserver(draw);
    ro.observe(canvas);
    return () => ro.disconnect();
  }, [draw]);

  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    const W = rect.width;

    for (const f of framesRef.current) {
      const px = f.x * W;
      const py = f.depth * ROW_HEIGHT;
      const pw = f.w * W;
      if (mx >= px && mx <= px + pw && my >= py && my <= py + ROW_HEIGHT) {
        setTooltip({ x: e.clientX, y: e.clientY, frame: f });
        return;
      }
    }
    setTooltip(null);
  }, []);

  const computedHeight = propHeight ?? (maxDepthRef.current + 1) * ROW_HEIGHT + ROW_HEIGHT;

  return (
    <div className={className} style={{ position: 'relative' }}>
      <canvas
        ref={canvasRef}
        style={{ width: '100%', height: computedHeight, cursor: 'pointer' }}
        onMouseMove={handleMouseMove}
        onMouseLeave={() => setTooltip(null)}
      />
      {tooltip && (
        <div
          className="fixed z-50 pointer-events-none rounded-lg border border-border bg-surface px-3 py-2 text-xs shadow-lg"
          style={{ left: tooltip.x + 12, top: tooltip.y - 8 }}
        >
          <div className="font-mono font-medium text-text-primary mb-1">{tooltip.frame.name}</div>
          <div className="text-text-secondary">
            Self: {((tooltip.frame.self / data.value) * 100).toFixed(1)}% &middot;
            Total: {((tooltip.frame.total / data.value) * 100).toFixed(1)}%
          </div>
        </div>
      )}
    </div>
  );
}
