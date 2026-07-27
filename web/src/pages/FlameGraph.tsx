import { useState } from 'react';
import { Search, Layers, Table2, SplitSquareVertical, GitCompare } from 'lucide-react';
import clsx from 'clsx';
import { FlameGraphCanvas } from '../components/charts/FlameGraphCanvas';
import { flameData } from '../mock/data';

type ViewMode = 'flame' | 'table' | 'both' | 'sandwich' | 'diff';

const viewModes: { key: ViewMode; icon: React.ReactNode; label: string }[] = [
  { key: 'flame', icon: <Layers size={14} />, label: 'Flame' },
  { key: 'table', icon: <Table2 size={14} />, label: 'Table' },
  { key: 'both', icon: <SplitSquareVertical size={14} />, label: 'Both' },
  { key: 'sandwich', icon: <Layers size={14} />, label: 'Sandwich' },
  { key: 'diff', icon: <GitCompare size={14} />, label: 'Diff' },
];

interface FlatRow {
  name: string;
  self: number;
  total: number;
  selfPct: string;
  totalPct: string;
}

function flattenForTable(frame: typeof flameData, root: number = flameData.value): FlatRow[] {
  const map = new Map<string, { self: number; total: number }>();

  function walk(f: typeof flameData) {
    const existing = map.get(f.name);
    if (existing) {
      existing.self += f.selfValue;
      existing.total += f.value;
    } else {
      map.set(f.name, { self: f.selfValue, total: f.value });
    }
    f.children.forEach(walk);
  }

  walk(frame);

  return Array.from(map.entries())
    .map(([name, { self, total }]) => ({
      name,
      self,
      total,
      selfPct: ((self / root) * 100).toFixed(1),
      totalPct: ((total / root) * 100).toFixed(1),
    }))
    .sort((a, b) => b.self - a.self);
}

export function FlameGraph() {
  const [viewMode, setViewMode] = useState<ViewMode>('flame');
  const [search, setSearch] = useState('');

  const rows = flattenForTable(flameData);
  const showFlame = viewMode === 'flame' || viewMode === 'both';
  const showTable = viewMode === 'table' || viewMode === 'both';

  return (
    <div className="page-container space-y-5">
      {/* Header */}
      <div className="animate-fade-in-up">
        <h1 className="text-xl font-bold">Flame Graph</h1>
        <p className="text-xs text-text-secondary mt-0.5">Interactive profiling visualization</p>
      </div>

      {/* Toolbar */}
      <div className="flex items-center gap-3 flex-wrap animate-fade-in-up" style={{ animationDelay: '80ms' }}>
        <div className="flex items-center gap-0.5 bg-surface-2 rounded-radius-sm p-0.5">
          {viewModes.map((m) => (
            <button
              key={m.key}
              onClick={() => setViewMode(m.key)}
              className={clsx(
                'flex items-center gap-1 px-2.5 py-1.5 rounded-md text-xs transition-colors cursor-pointer',
                viewMode === m.key
                  ? 'bg-accent text-white'
                  : 'text-text-secondary hover:text-text-primary'
              )}
            >
              {m.icon}
              {m.label}
            </button>
          ))}
        </div>

        <div className="relative flex-1 max-w-xs">
          <Search size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-text-muted" />
          <input
            type="text"
            placeholder="Search functions..."
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            className="w-full pl-8 pr-3 py-2 bg-surface-2 border border-border rounded-radius-sm text-xs text-text-primary placeholder:text-text-muted focus:outline-none focus:border-accent/50 transition-colors"
          />
        </div>
      </div>

      {/* Flame graph */}
      {showFlame && (
        <div className="rounded-radius-md border border-border bg-surface p-3 animate-fade-in-up" style={{ animationDelay: '160ms' }}>
          <FlameGraphCanvas data={flameData} height={Math.max(300, 22 * 12)} />
        </div>
      )}

      {/* Table */}
      {showTable && (
        <div className="rounded-radius-md border border-border bg-surface animate-fade-in-up overflow-hidden" style={{ animationDelay: '240ms' }}>
          <table className="w-full">
            <thead>
              <tr className="border-b border-border text-[11px] text-text-muted uppercase tracking-wider">
                <th className="px-4 py-2.5 text-left font-medium">Function</th>
                <th className="px-4 py-2.5 text-right font-medium">Self</th>
                <th className="px-4 py-2.5 text-right font-medium">Self %</th>
                <th className="px-4 py-2.5 text-right font-medium">Total</th>
                <th className="px-4 py-2.5 text-right font-medium">Total %</th>
              </tr>
            </thead>
            <tbody>
              {rows
                .filter((r) => !search || r.name.toLowerCase().includes(search.toLowerCase()))
                .map((r) => (
                  <tr key={r.name} className="border-b border-border-subtle hover:bg-surface-2 transition-colors text-xs font-mono">
                    <td className="px-4 py-2 text-text-primary truncate max-w-[300px]">{r.name}</td>
                    <td className="px-4 py-2 text-right text-text-secondary">{r.self}</td>
                    <td className="px-4 py-2 text-right text-text-primary">{r.selfPct}%</td>
                    <td className="px-4 py-2 text-right text-text-secondary">{r.total}</td>
                    <td className="px-4 py-2 text-right text-text-primary">{r.totalPct}%</td>
                  </tr>
                ))}
            </tbody>
          </table>
        </div>
      )}

      {/* Info bar */}
      <div className="flex items-center gap-4 text-xs text-text-muted animate-fade-in-up rounded-radius-md border border-border bg-surface px-4 py-3" style={{ animationDelay: '320ms' }}>
        <span>Total samples: <span className="font-mono text-text-secondary">{flameData.value}</span></span>
        <span>Functions: <span className="font-mono text-text-secondary">{rows.length}</span></span>
        <span>Max depth: <span className="font-mono text-text-secondary">6</span></span>
      </div>
    </div>
  );
}
