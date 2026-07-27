import { Play, Pause, Square, Settings } from 'lucide-react';
import clsx from 'clsx';
import { SparkLine } from '../charts/SparkLine';
import type { Feature, FeatureStatus } from '../../mock/data';
import { useFeatureStore } from '../../stores/feature-store';

interface FeatureCardProps {
  feature: Feature;
  index?: number;
  onNavigate?: (name: string) => void;
}

const statusColors: Record<FeatureStatus, string> = {
  active: 'bg-status-active',
  paused: 'bg-status-paused',
  inactive: 'bg-status-inactive',
  error: 'bg-status-error',
};

const tierStyles: Record<string, string> = {
  monitoring: 'bg-indigo-500/15 text-indigo-400',
  profiling: 'bg-purple-500/15 text-purple-400',
  tracing: 'bg-orange-500/15 text-orange-400',
};

export function FeatureCard({ feature, index = 0, onNavigate }: FeatureCardProps) {
  const setStatus = useFeatureStore((s) => s.setStatus);

  const handleToggle = (e: React.MouseEvent) => {
    e.stopPropagation();
    if (feature.status === 'active') setStatus(feature.name, 'paused');
    else if (feature.status === 'paused' || feature.status === 'inactive')
      setStatus(feature.name, 'active');
  };

  const handleStop = (e: React.MouseEvent) => {
    e.stopPropagation();
    setStatus(feature.name, 'inactive');
  };

  return (
    <div
      className={clsx(
        'group rounded-radius-md border border-border bg-surface transition-all duration-300 cursor-pointer',
        'hover:border-accent/30 card-glow animate-fade-in-up min-w-0 flex flex-col'
      )}
      style={{ animationDelay: `${index * 80}ms` }}
      onClick={() => onNavigate?.(feature.name)}
    >
      <div className="p-5 sm:p-6 flex-1">
        <div className="flex items-center gap-3 mb-4">
          <span
            className={clsx(
              'w-2.5 h-2.5 rounded-full flex-shrink-0',
              statusColors[feature.status],
              feature.status === 'active' && 'shadow-[0_0_6px_currentColor]'
            )}
            style={feature.status === 'active' ? { color: '#22c55e', animation: 'pulse-glow 2s ease-in-out infinite' } : undefined}
          />
          <span className="text-base font-semibold text-text-primary truncate">
            {feature.displayName}
          </span>
          <span className={clsx('ml-auto text-xs font-medium px-2.5 py-1 rounded-full flex-shrink-0', tierStyles[feature.tier])}>
            {feature.tier}
          </span>
        </div>

        <div className="flex items-end justify-between gap-4">
          <div className="min-w-0">
            <div className="text-xl font-bold font-mono text-text-primary leading-tight">
              {feature.summary}
            </div>
            {feature.interval && (
              <div className="text-xs text-text-muted mt-1.5">
                every {feature.interval}
              </div>
            )}
          </div>
          <div className="flex-shrink-0 w-[100px]">
            <SparkLine data={feature.sparkData} height={36} />
          </div>
        </div>
      </div>

      <div className="border-t border-border px-5 sm:px-6 py-3 flex items-center gap-2">
        <ActionBtn
          icon={feature.status === 'active' ? <Pause size={13} /> : <Play size={13} />}
          label={feature.status === 'active' ? 'Pause' : 'Start'}
          onClick={handleToggle}
        />
        <ActionBtn icon={<Square size={13} />} label="Stop" onClick={handleStop} />
        <ActionBtn icon={<Settings size={13} />} label="Config" onClick={(e) => e.stopPropagation()} className="ml-auto" />
      </div>
    </div>
  );
}

function ActionBtn({
  icon,
  label,
  onClick,
  className,
}: {
  icon: React.ReactNode;
  label: string;
  onClick: (e: React.MouseEvent) => void;
  className?: string;
}) {
  return (
    <button
      className={clsx(
        'flex items-center gap-1.5 px-3 py-1.5 rounded-radius-sm text-xs text-text-secondary',
        'hover:bg-surface-3 hover:text-text-primary transition-colors cursor-pointer',
        className
      )}
      onClick={onClick}
    >
      {icon}
      <span>{label}</span>
    </button>
  );
}
