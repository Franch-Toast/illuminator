import { TrendingUp, TrendingDown, Minus } from 'lucide-react';
import clsx from 'clsx';
import { CountUp } from '../effects/CountUp';
import { SparkLine } from '../charts/SparkLine';
import type { GoldenSignal } from '../../mock/data';

interface StatCardProps {
  signal: GoldenSignal;
  index?: number;
}

const trendIcon = {
  up: <TrendingUp size={14} className="text-status-error" />,
  down: <TrendingDown size={14} className="text-status-active" />,
  stable: <Minus size={14} className="text-text-muted" />,
};

export function StatCard({ signal, index = 0 }: StatCardProps) {
  const decimals = signal.value < 1 ? 3 : signal.value < 100 ? 1 : 0;

  return (
    <div
      className={clsx(
        'group rounded-radius-md border border-border bg-surface p-5 sm:p-6 transition-all duration-300',
        'hover:border-accent/30 card-glow animate-fade-in-up min-w-0'
      )}
      style={{ animationDelay: `${index * 80}ms` }}
    >
      <div className="flex items-center justify-between mb-4">
        <span className="text-xs font-medium uppercase tracking-wider text-text-muted">
          {signal.label}
        </span>
        {trendIcon[signal.trend]}
      </div>

      <div className="flex items-end justify-between gap-4">
        <div className="min-w-0">
          <CountUp
            end={signal.value}
            decimals={decimals}
            className="text-[2.25rem] font-bold leading-none font-mono text-text-primary"
          />
          <span className="ml-2 text-sm text-text-muted">{signal.unit}</span>
        </div>

        <div className="flex-shrink-0 w-[100px]">
          <SparkLine data={signal.sparkData} height={36} />
        </div>
      </div>
    </div>
  );
}
