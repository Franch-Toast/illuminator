import { useParams, useNavigate } from 'react-router-dom';
import { ArrowLeft, Pause, Play, Settings, RotateCcw } from 'lucide-react';
import clsx from 'clsx';
import { useFeatureStore } from '../stores/feature-store';
import { TimeSeriesChart } from '../components/charts/TimeSeriesChart';
import { cpuTimeSeriesData, processTableData } from '../mock/data';

const statusStyle: Record<string, string> = {
  active: 'bg-status-active/15 text-status-active',
  paused: 'bg-status-paused/15 text-status-paused',
  inactive: 'bg-status-inactive/15 text-status-inactive',
  error: 'bg-status-error/15 text-status-error',
};

export function FeatureDetail() {
  const { name } = useParams<{ name: string }>();
  const navigate = useNavigate();
  const feature = useFeatureStore((s) => s.features.find((f) => f.name === name));
  const setStatus = useFeatureStore((s) => s.setStatus);

  if (!feature) {
    return (
      <div className="page-container flex items-center justify-center h-full text-text-muted">
        Feature not found
      </div>
    );
  }

  const series = [
    { name: 'user', data: cpuTimeSeriesData.user, color: '#6366f1' },
    { name: 'system', data: cpuTimeSeriesData.system, color: '#f97316' },
    { name: 'iowait', data: cpuTimeSeriesData.iowait, color: '#ef4444' },
    { name: 'idle', data: cpuTimeSeriesData.idle, color: '#22c55e' },
  ];

  const handleToggle = () => {
    if (feature.status === 'active') setStatus(feature.name, 'paused');
    else setStatus(feature.name, 'active');
  };

  return (
    <div className="page-container space-y-6">
      {/* Header */}
      <div className="flex items-center gap-4 animate-fade-in-up">
        <button onClick={() => navigate(-1)} className="p-2 rounded-radius-sm hover:bg-surface-2 transition-colors cursor-pointer text-text-secondary">
          <ArrowLeft size={18} />
        </button>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-3">
            <h1 className="text-xl font-bold truncate">{feature.displayName}</h1>
            <span className={clsx('text-[11px] font-semibold px-2.5 py-0.5 rounded-full', statusStyle[feature.status])}>
              {feature.status}
            </span>
          </div>
          <p className="text-sm text-text-secondary mt-0.5 font-mono">{feature.summary}</p>
        </div>

        <div className="flex items-center gap-2">
          <CtrlBtn icon={feature.status === 'active' ? <Pause size={14} /> : <Play size={14} />} label={feature.status === 'active' ? 'Pause' : 'Resume'} onClick={handleToggle} />
          <CtrlBtn icon={<Settings size={14} />} label="Configure" />
          <CtrlBtn icon={<RotateCcw size={14} />} label="Reset" />
        </div>
      </div>

      {/* Main grid */}
      <div className="grid grid-cols-1 2xl:grid-cols-[1fr_400px] gap-4">
        {/* Chart area */}
        <div className="rounded-radius-md border border-border bg-surface p-4 animate-fade-in-up" style={{ animationDelay: '100ms' }}>
          <h3 className="text-sm font-semibold mb-3">Time Series</h3>
          <TimeSeriesChart series={series} height={340} />
        </div>

        {/* Side panel */}
        <div className="rounded-radius-md border border-border bg-surface animate-fade-in-up" style={{ animationDelay: '200ms' }}>
          <div className="px-4 py-3 border-b border-border">
            <h3 className="text-sm font-semibold">Top Processes</h3>
          </div>
          <div className="overflow-auto max-h-[400px]">
            <table className="w-full">
              <thead>
                <tr className="border-b border-border text-[11px] text-text-muted uppercase tracking-wider">
                  <th className="px-4 py-2.5 text-left font-medium">PID</th>
                  <th className="px-4 py-2.5 text-left font-medium">Command</th>
                  <th className="px-4 py-2.5 text-right font-medium">CPU%</th>
                  <th className="px-4 py-2.5 text-right font-medium">MEM</th>
                </tr>
              </thead>
              <tbody>
                {processTableData.map((p) => (
                  <tr key={p.pid} className="border-b border-border-subtle hover:bg-surface-2 transition-colors text-xs font-mono">
                    <td className="px-4 py-2.5 text-text-muted">{p.pid}</td>
                    <td className="px-4 py-2.5 text-text-primary">{p.comm}</td>
                    <td className="px-4 py-2.5 text-right text-text-primary">{p.cpu.toFixed(1)}</td>
                    <td className="px-4 py-2.5 text-right text-text-secondary">{p.mem}M</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      </div>

      {/* Bottom tabs */}
      <div className="rounded-radius-md border border-border bg-surface animate-fade-in-up" style={{ animationDelay: '300ms' }}>
        <div className="flex border-b border-border">
          {['Raw Data', 'BPF Stats', 'Configuration'].map((tab, i) => (
            <button
              key={tab}
              className={clsx(
                'px-4 py-3 text-xs font-medium transition-colors cursor-pointer',
                i === 0 ? 'text-accent border-b-2 border-accent' : 'text-text-muted hover:text-text-secondary'
              )}
            >
              {tab}
            </button>
          ))}
        </div>
        <pre className="p-4 text-xs font-mono text-text-secondary overflow-auto max-h-48">
{JSON.stringify({ feature: feature.name, status: feature.status, config: feature.config }, null, 2)}
        </pre>
      </div>
    </div>
  );
}

function CtrlBtn({ icon, label, onClick }: { icon: React.ReactNode; label: string; onClick?: () => void }) {
  return (
    <button
      onClick={onClick}
      className="flex items-center gap-1.5 px-3 py-2 text-xs font-medium text-text-secondary border border-border rounded-radius-sm hover:bg-surface-2 hover:text-text-primary transition-colors cursor-pointer"
    >
      {icon}
      {label}
    </button>
  );
}
