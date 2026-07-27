import { useState } from 'react';
import { Puzzle, Check, RefreshCw } from 'lucide-react';
import clsx from 'clsx';
import { useFeatureStore } from '../stores/feature-store';
import type { FeatureStatus } from '../mock/data';

const statusColors: Record<FeatureStatus, string> = {
  active: 'bg-status-active',
  paused: 'bg-status-paused',
  inactive: 'bg-status-inactive',
  error: 'bg-status-error',
};

export function PluginManager() {
  const features = useFeatureStore((s) => s.features);
  const setStatus = useFeatureStore((s) => s.setStatus);
  const [selected, setSelected] = useState<string | null>(features[0]?.name ?? null);

  const current = features.find((f) => f.name === selected);

  return (
    <div className="page-container space-y-6">
      <div className="flex items-center gap-3 animate-fade-in-up">
        <Puzzle size={20} className="text-accent" />
        <div>
          <h1 className="text-xl font-bold">Plugin Manager</h1>
          <p className="text-xs text-text-secondary mt-0.5">Manage features and their configurations</p>
        </div>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-[1fr_400px] gap-4">
        {/* Feature list */}
        <div className="rounded-radius-md border border-border bg-surface animate-fade-in-up" style={{ animationDelay: '80ms' }}>
          <div className="flex items-center justify-between px-4 py-3 border-b border-border">
            <h3 className="text-sm font-semibold">Registered Features</h3>
            <button className="flex items-center gap-1.5 px-3 py-1.5 text-xs text-text-secondary border border-border rounded-radius-sm hover:bg-surface-2 transition-colors cursor-pointer">
              <RefreshCw size={13} />
              Reload
            </button>
          </div>

          <div className="divide-y divide-border-subtle">
            {features.map((f) => (
              <button
                key={f.name}
                onClick={() => setSelected(f.name)}
                className={clsx(
                  'w-full flex items-center gap-3 px-4 py-3.5 text-left transition-colors cursor-pointer',
                  selected === f.name ? 'bg-accent/8' : 'hover:bg-surface-2'
                )}
              >
                <span className={clsx('w-2.5 h-2.5 rounded-full flex-shrink-0', statusColors[f.status])} />
                <div className="flex-1 min-w-0">
                  <div className="text-sm font-medium text-text-primary truncate">{f.displayName}</div>
                  <div className="text-[11px] text-text-muted font-mono truncate">{f.name}</div>
                </div>
                <span className="text-[10px] text-text-muted capitalize">{f.status}</span>
              </button>
            ))}
          </div>
        </div>

        {/* Config panel */}
        <div className="rounded-radius-md border border-border bg-surface animate-fade-in-up" style={{ animationDelay: '160ms' }}>
          {current ? (
            <>
              <div className="px-4 py-3 border-b border-border">
                <h3 className="text-sm font-semibold">{current.displayName}</h3>
                <p className="text-[11px] text-text-muted mt-0.5">{current.tier} &middot; {current.modelType}</p>
              </div>

              <div className="p-4 space-y-4">
                {/* Status toggle */}
                <div className="flex items-center justify-between">
                  <span className="text-xs text-text-secondary">Enabled</span>
                  <button
                    onClick={() =>
                      setStatus(current.name, current.status === 'active' ? 'inactive' : 'active')
                    }
                    className={clsx(
                      'w-10 h-[22px] rounded-full transition-colors cursor-pointer relative',
                      current.status === 'active' ? 'bg-accent' : 'bg-surface-3'
                    )}
                  >
                    <span
                      className={clsx(
                        'absolute top-0.5 w-[18px] h-[18px] rounded-full bg-white transition-transform',
                        current.status === 'active' ? 'translate-x-[20px]' : 'translate-x-0.5'
                      )}
                    />
                  </button>
                </div>

                {/* Config fields */}
                {Object.entries(current.config).map(([key, val]) => (
                  <div key={key}>
                    <label className="block text-[11px] text-text-muted mb-1 font-mono">{key}</label>
                    {typeof val === 'boolean' ? (
                      <button className={clsx('w-10 h-[22px] rounded-full transition-colors cursor-pointer relative', val ? 'bg-accent' : 'bg-surface-3')}>
                        <span className={clsx('absolute top-0.5 w-[18px] h-[18px] rounded-full bg-white transition-transform', val ? 'translate-x-[20px]' : 'translate-x-0.5')} />
                      </button>
                    ) : (
                      <input
                        className="w-full px-3 py-2 bg-surface-2 border border-border rounded-radius-sm text-xs text-text-primary font-mono focus:outline-none focus:border-accent/50 transition-colors"
                        defaultValue={typeof val === 'object' ? JSON.stringify(val) : String(val)}
                      />
                    )}
                  </div>
                ))}

                <button className="w-full flex items-center justify-center gap-2 px-4 py-2.5 bg-accent hover:bg-accent-dim text-white text-sm font-medium rounded-radius-sm transition-colors cursor-pointer mt-4">
                  <Check size={14} />
                  Apply Configuration
                </button>
              </div>
            </>
          ) : (
            <div className="flex items-center justify-center h-48 text-text-muted text-sm">
              Select a feature to configure
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
