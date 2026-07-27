import { useState, useMemo, useCallback, useEffect } from 'react';
import { ArrowLeft, Cpu, ChevronDown, ChevronRight, X, Pause, Play } from 'lucide-react';
import { useNavigate } from 'react-router-dom';
import clsx from 'clsx';
import { useTimeSeriesStore } from '../stores/timeseries-store';
import { TimeSeriesChart } from '../components/charts/TimeSeriesChart';
import { SparkLine } from '../components/charts/SparkLine';
import { CountUp } from '../components/effects/CountUp';

const TABS = ['System', 'Processes'] as const;
type Tab = typeof TABS[number];

type FeatureState = 'active' | 'paused' | 'inactive' | 'unknown';

async function fetchFeatureState(name: string): Promise<FeatureState> {
  try {
    const res = await fetch(`/api/v2/features/${name}/stats`);
    if (!res.ok) return 'unknown';
    const data = await res.json();
    return (data.state as FeatureState) ?? 'unknown';
  } catch { return 'unknown'; }
}

async function toggleFeature(name: string, action: 'pause' | 'resume'): Promise<boolean> {
  try {
    const res = await fetch(`/api/v2/features/${name}/${action}`, { method: 'POST' });
    return res.ok;
  } catch { return false; }
}

export function CpuPage() {
  const [activeTab, setActiveTab] = useState<Tab>('System');
  const [cpuState, setCpuState] = useState<FeatureState>('unknown');
  const [procState, setProcState] = useState<FeatureState>('unknown');
  const [toggling, setToggling] = useState(false);
  const navigate = useNavigate();

  useEffect(() => {
    fetchFeatureState('cpu_utilization').then(setCpuState);
    fetchFeatureState('process_cpu').then(setProcState);
  }, []);

  const overallState: FeatureState =
    cpuState === 'active' || procState === 'active' ? 'active'
    : cpuState === 'paused' && procState === 'paused' ? 'paused'
    : 'unknown';

  const handleToggle = async () => {
    if (toggling) return;
    setToggling(true);
    const action = overallState === 'active' ? 'pause' : 'resume';
    await Promise.all([
      toggleFeature('cpu_utilization', action),
      toggleFeature('process_cpu', action),
    ]);
    const [s1, s2] = await Promise.all([
      fetchFeatureState('cpu_utilization'),
      fetchFeatureState('process_cpu'),
    ]);
    setCpuState(s1);
    setProcState(s2);
    setToggling(false);
  };

  return (
    <div className="page-container space-y-8">
      {/* Header */}
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-4">
          <button onClick={() => navigate('/dashboard')}
            className="p-2.5 rounded-xl hover:bg-white/5 text-text-muted transition-colors">
            <ArrowLeft size={20} />
          </button>
          <div className="flex items-center gap-3.5">
            <div className="p-2.5 rounded-xl bg-accent/10">
              <Cpu size={24} className="text-accent" />
            </div>
            <div>
              <h1 className="text-2xl sm:text-3xl font-bold tracking-tight text-text-primary">CPU Monitor</h1>
              <p className="text-sm text-text-muted mt-1">System &amp; Process CPU Utilization</p>
            </div>
          </div>
        </div>
        <div className="flex items-center gap-3">
          <span className={clsx('text-xs font-semibold uppercase tracking-wider px-3 py-1 rounded-full',
            overallState === 'active' ? 'bg-green-500/10 text-green-400' :
            overallState === 'paused' ? 'bg-yellow-500/10 text-yellow-400' :
            'bg-white/5 text-text-muted'
          )}>
            {overallState === 'active' ? 'Collecting' : overallState === 'paused' ? 'Paused' : '—'}
          </span>
          <button onClick={handleToggle} disabled={toggling || overallState === 'unknown'}
            className={clsx(
              'flex items-center gap-2 px-4 py-2 rounded-xl text-sm font-semibold transition-all',
              overallState === 'active'
                ? 'bg-yellow-500/10 text-yellow-400 hover:bg-yellow-500/20'
                : 'bg-green-500/10 text-green-400 hover:bg-green-500/20',
              (toggling || overallState === 'unknown') && 'opacity-50 cursor-not-allowed'
            )}>
            {toggling ? (
              <span className="animate-pulse">...</span>
            ) : overallState === 'active' ? (
              <><Pause size={14} /> Pause</>
            ) : (
              <><Play size={14} /> Resume</>
            )}
          </button>
        </div>
      </div>

      {/* Sub-tabs */}
      <div className="flex gap-1.5 p-1.5 bg-white/[0.03] rounded-xl border border-white/[0.06] w-fit">
        {TABS.map((tab) => (
          <button key={tab} onClick={() => setActiveTab(tab)}
            className={clsx(
              'px-7 py-2.5 rounded-lg text-sm font-semibold tracking-wide transition-all',
              activeTab === tab
                ? 'bg-accent/15 text-accent shadow-sm shadow-accent/10'
                : 'text-text-muted hover:text-text-secondary hover:bg-white/[0.04]'
            )}>
            {tab}
          </button>
        ))}
      </div>

      {/* Tab content */}
      {activeTab === 'System' ? <SystemTab /> : <ProcessesTab />}
    </div>
  );
}

// ====================================================================
// System Tab — 系统级 CPU 指标
// ====================================================================
function SystemTab() {
  const seriesMap = useTimeSeriesStore((s) => s.series);
  const sel = (key: string) => seriesMap.get(key)?.data ?? [];

  const busyData = sel('cpu_utilization/system_total/busy_pct');
  const userData = sel('cpu_utilization/system_total/user_pct');
  const sysData = sel('cpu_utilization/system_total/system_pct');
  const iowaitData = sel('cpu_utilization/system_total/iowait_pct');
  const irqData = sel('cpu_utilization/system_total/irq_pct');
  const loadData = sel('cpu_utilization/system_total/load_1m');
  const ctxtData = sel('cpu_utilization/system_total/ctxt_per_sec');
  const runData = sel('cpu_utilization/system_total/procs_running');
  const blkData = sel('cpu_utilization/system_total/procs_blocked');

  const latest = useMemo(() => {
    const last = (d: typeof busyData) => d.length > 0 ? d[d.length - 1].value : 0;
    return {
      busy: last(busyData),
      load: last(loadData),
      ctxt: last(ctxtData),
      running: last(runData),
      blocked: last(blkData),
    };
  }, [busyData, loadData, ctxtData, runData, blkData]);

  const sparkVals = useCallback((data: typeof busyData) =>
    data.slice(-60).map((d) => d.value), []);

  const chartSeries = useMemo(() => [
    { name: 'user', data: userData, color: '#6366f1' },
    { name: 'system', data: sysData, color: '#f97316' },
    { name: 'iowait', data: iowaitData, color: '#ef4444' },
    { name: 'irq', data: irqData, color: '#eab308' },
  ], [userData, sysData, iowaitData, irqData]);

  return (
    <div className="space-y-6">
      {/* Stat Cards */}
      <div className="grid grid-cols-2 xl:grid-cols-4 gap-5">
        <StatMini label="CPU Busy" value={latest.busy} unit="%"
          sparkData={sparkVals(busyData)} warn={70} crit={90} />
        <StatMini label="Load Avg (1m)" value={latest.load} unit=""
          sparkData={sparkVals(loadData)} />
        <StatMini label="Context Switch" value={latest.ctxt} unit="/s"
          sparkData={sparkVals(ctxtData)} format="compact" />
        <StatMini label="Running / Blocked"
          value={latest.running} unit=""
          secondary={`/ ${Math.round(latest.blocked)}`}
          sparkData={sparkVals(runData)} />
      </div>

      {/* Stacked Area Chart */}
      <div className="card-glow rounded-2xl p-6">
        <h3 className="text-sm font-semibold text-text-secondary mb-4 uppercase tracking-widest">CPU Distribution</h3>
        <TimeSeriesChart series={chartSeries} height={340}
          thresholds={[
            { value: 70, color: '#eab308', label: 'Warning 70%' },
            { value: 90, color: '#ef4444', label: 'Critical 90%' },
          ]} />
      </div>
    </div>
  );
}

// ====================================================================
// Processes Tab — 进程级 CPU 指标
// ====================================================================
function ProcessesTab() {
  const seriesMap = useTimeSeriesStore((s) => s.series);
  const processComms = useTimeSeriesStore((s) => s.processComms);
  const [search, setSearch] = useState('');
  const [expandedPid, setExpandedPid] = useState<string | null>(null);

  const enrichedProcesses = useMemo(() => {
    const pidMap = new Map<string, Record<string, number>>();
    const prefix = 'process_cpu/';

    for (const [key, buf] of seriesMap.entries()) {
      if (!key.startsWith(prefix)) continue;
      const rest = key.slice(prefix.length);
      const parts = rest.split('/');
      if (parts.length < 2) continue;
      const labelPart = parts[0];
      const field = parts[1];

      if (!labelPart.startsWith('process:')) continue;
      const pid = labelPart.split(':')[1];

      const lastVal = buf.data.length > 0 ? buf.data[buf.data.length - 1].value : 0;

      if (!pidMap.has(pid)) pidMap.set(pid, {});
      pidMap.get(pid)![field] = lastVal;
    }

    return Array.from(pidMap.entries()).map(([pid, fields]) => ({
      pid,
      comm: processComms.get(pid) ?? `pid-${pid}`,
      cpu_total: fields['cpu_total_pct'] ?? 0,
      cpu_user: fields['cpu_user_pct'] ?? 0,
      cpu_sys: fields['cpu_sys_pct'] ?? 0,
      threads: fields['num_threads'] ?? 0,
      rss_kb: fields['rss_kb'] ?? 0,
      state: '',
    }));
  }, [seriesMap, processComms]);

  const filtered = useMemo(() => {
    let list = enrichedProcesses;
    if (search) {
      const q = search.toLowerCase();
      list = list.filter((p) => p.comm.toLowerCase().includes(q) || p.pid.includes(q));
    }
    list.sort((a, b) => b.cpu_total - a.cpu_total);
    return list;
  }, [enrichedProcesses, search]);

  const sparkVals = useCallback((pid: string) => {
    const buf = seriesMap.get(`process_cpu/process:${pid}/cpu_total_pct`);
    return (buf?.data ?? []).slice(-30).map((d) => d.value);
  }, [seriesMap]);

  return (
    <div className="space-y-5">
      {/* Controls */}
      <div className="flex flex-wrap items-center gap-4">
        <input type="text" value={search} onChange={(e) => setSearch(e.target.value)}
          placeholder="Search process name or PID..."
          className="flex-1 min-w-[200px] max-w-sm px-4 py-2.5 bg-white/[0.04] border border-white/[0.08]
                     rounded-xl text-sm text-text-primary placeholder:text-text-muted/50 focus:outline-none
                     focus:border-accent/40 focus:ring-1 focus:ring-accent/20 transition-all" />
        <span className="text-xs text-text-muted ml-auto tabular-nums">
          {filtered.length} processes · sorted by CPU%
        </span>
      </div>

      {/* Process Table */}
      <div className="card-glow rounded-2xl overflow-hidden">
        <table className="w-full text-sm">
          <thead>
            <tr className="border-b border-white/[0.08] bg-white/[0.02]">
              {['PID', 'Name', 'CPU%', 'User%', 'Sys%', 'Trend', 'State'].map((h) => (
                <th key={h} className="px-5 py-3.5 text-left text-[11px] font-semibold text-text-muted uppercase tracking-widest">
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody className="divide-y divide-white/[0.04]">
            {filtered.length === 0 ? (
              <tr>
                <td colSpan={7} className="px-5 py-12 text-center text-text-muted">
                  <Cpu size={32} className="mx-auto mb-3 opacity-30" />
                  <p>Waiting for process data...</p>
                </td>
              </tr>
            ) : filtered.map((p) => {
              const isExpanded = expandedPid === p.pid;
              return (
                <ProcessRow key={p.pid} proc={p} isExpanded={isExpanded}
                  sparkVals={sparkVals(p.pid)}
                  onToggle={() => setExpandedPid(isExpanded ? null : p.pid)} />
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ====================================================================
// StatMini — 顶部统计小卡片
// ====================================================================
interface StatMiniProps {
  label: string;
  value: number;
  unit: string;
  sparkData: number[];
  warn?: number;
  crit?: number;
  secondary?: string;
  format?: 'default' | 'compact';
}

function StatMini({ label, value, unit, sparkData, warn, crit, secondary, format }: StatMiniProps) {
  const color = crit && value >= crit ? 'text-red-400'
    : warn && value >= warn ? 'text-yellow-400' : 'text-text-primary';

  const displayVal = format === 'compact' && value >= 1000
    ? `${(value / 1000).toFixed(1)}k` : undefined;

  const ringColor = crit && value >= crit ? 'ring-red-500/20'
    : warn && value >= warn ? 'ring-yellow-500/20' : 'ring-transparent';

  return (
    <div className={clsx('card-glow rounded-2xl p-5 sm:p-6 ring-1 transition-all', ringColor)}>
      <p className="text-[11px] text-text-muted mb-3 uppercase tracking-widest font-semibold">{label}</p>
      <div className="flex items-end justify-between gap-4">
        <div className="flex items-baseline gap-2">
          <span className={clsx('text-3xl sm:text-4xl font-bold tabular-nums tracking-tight', color)}>
            {displayVal ?? <CountUp end={value} decimals={value < 10 ? 1 : 0} />}
          </span>
          {unit && <span className="text-sm text-text-muted font-medium">{unit}</span>}
          {secondary && <span className="text-xl text-text-muted font-medium">{secondary}</span>}
        </div>
        <SparkLine data={sparkData} width={100} height={32}
          color={crit && value >= crit ? '#f87171' : warn && value >= warn ? '#facc15' : undefined} />
      </div>
    </div>
  );
}

// ====================================================================
// ProcessRow — 可展开的进程行 + 线程详情
// ====================================================================
interface ProcessData {
  pid: string;
  comm: string;
  cpu_total: number;
  cpu_user: number;
  cpu_sys: number;
  threads: number;
  rss_kb: number;
  state: string;
}

function ProcessRow({ proc: p, isExpanded, sparkVals, onToggle }: {
  proc: ProcessData;
  isExpanded: boolean;
  sparkVals: number[];
  onToggle: () => void;
}) {
  return (
    <>
      <tr onClick={onToggle}
        className={clsx(
          'group transition-colors cursor-pointer',
          isExpanded ? 'bg-accent/[0.04]' : 'hover:bg-white/[0.03]'
        )}>
        <td className="px-5 py-3.5">
          <div className="flex items-center gap-2">
            {isExpanded
              ? <ChevronDown size={14} className="text-accent" />
              : <ChevronRight size={14} className="text-text-muted group-hover:text-text-secondary transition-colors" />}
            <span className="font-mono text-xs text-text-muted">{p.pid}</span>
          </div>
        </td>
        <td className="px-5 py-3.5 font-semibold text-text-primary">{p.comm}</td>
        <td className="px-5 py-3.5">
          <div className="flex items-center gap-2">
            <div className="w-16 h-1.5 rounded-full bg-white/[0.06] overflow-hidden">
              <div className={clsx(
                'h-full rounded-full transition-all duration-500',
                p.cpu_total > 80 ? 'bg-red-500' : p.cpu_total > 50 ? 'bg-yellow-500' : 'bg-accent'
              )} style={{ width: `${Math.min(100, p.cpu_total)}%` }} />
            </div>
            <span className={clsx(
              'font-mono font-bold text-xs tabular-nums',
              p.cpu_total > 80 ? 'text-red-400' : p.cpu_total > 50 ? 'text-yellow-400' : 'text-text-primary'
            )}>
              {p.cpu_total.toFixed(1)}%
            </span>
          </div>
        </td>
        <td className="px-5 py-3.5 font-mono text-xs text-text-secondary tabular-nums">{p.cpu_user.toFixed(1)}</td>
        <td className="px-5 py-3.5 font-mono text-xs text-text-secondary tabular-nums">{p.cpu_sys.toFixed(1)}</td>
        <td className="px-5 py-3.5 w-28">
          <SparkLine data={sparkVals} width={90} height={26}
            color={p.cpu_total > 50 ? '#f87171' : 'var(--color-accent)'} />
        </td>
        <td className="px-5 py-3.5">
          <span className={clsx(
            'inline-flex items-center justify-center w-6 h-6 rounded-md text-[10px] font-bold',
            p.state === 'R' ? 'bg-green-500/15 text-green-400 ring-1 ring-green-500/20' :
            p.state === 'D' ? 'bg-red-500/15 text-red-400 ring-1 ring-red-500/20' :
            'bg-white/[0.04] text-text-muted ring-1 ring-white/[0.06]'
          )}>
                    {p.state || 'S'}
                  </span>
                </td>
              </tr>
      {isExpanded && (
        <tr>
          <td colSpan={7} className="p-0">
            <ThreadDrawer pid={p.pid} comm={p.comm} onClose={onToggle} />
          </td>
        </tr>
      )}
    </>
  );
}

// ====================================================================
// ThreadDrawer — 线程级 CPU 详情面板（真实后端数据）
// ====================================================================
interface ThreadInfo {
  tid: number;
  comm: string;
  state: string;
  utime: number;
  stime: number;
}

function ThreadDrawer({ pid, comm, onClose }: { pid: string; comm: string; onClose: () => void }) {
  const [threads, setThreads] = useState<ThreadInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setError(null);

    fetch(`/api/v2/features/process_cpu/query?q=threads&pid=${pid}`)
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return res.json();
      })
      .then((data) => {
        if (cancelled) return;
        if (data.error) { setError(data.error); return; }
        const raw: ThreadInfo[] = data.threads ?? [];
        const total = raw.reduce((s, t) => s + t.utime + t.stime, 0) || 1;
        const enriched = raw.map((t) => ({
          ...t,
          cpu_pct: ((t.utime + t.stime) / total) * 100,
          user_pct: (t.utime / total) * 100,
          sys_pct: (t.stime / total) * 100,
        })).sort((a, b) => b.cpu_pct - a.cpu_pct);
        setThreads(enriched as any);
      })
      .catch((e) => { if (!cancelled) setError(e.message); })
      .finally(() => { if (!cancelled) setLoading(false); });

    return () => { cancelled = true; };
  }, [pid]);

  const enrichedThreads = threads as (ThreadInfo & { cpu_pct: number; user_pct: number; sys_pct: number })[];

  return (
    <div className="bg-accent/[0.02] border-t border-b border-accent/10 animate-fade-in-up">
      <div className="px-6 py-4">
        <div className="flex items-center justify-between mb-4">
          <div className="flex items-center gap-3">
            <span className="text-xs font-bold text-accent uppercase tracking-wider">Thread Details</span>
            <span className="text-xs text-text-muted">
              {comm} (PID {pid}) — {enrichedThreads.length} threads
            </span>
          </div>
          <button onClick={onClose} className="p-1.5 rounded-lg hover:bg-white/[0.06] text-text-muted transition-colors">
            <X size={14} />
          </button>
        </div>
        {loading ? (
          <div className="py-8 text-center text-text-muted text-xs animate-pulse">Loading threads...</div>
        ) : error ? (
          <div className="py-8 text-center text-red-400 text-xs">{error}</div>
        ) : (
        <div className="rounded-xl overflow-hidden border border-white/[0.06]">
          <table className="w-full text-xs">
            <thead>
              <tr className="bg-white/[0.03]">
                {['TID', 'Thread Name', 'CPU%', 'User%', 'Sys%', 'State'].map((h) => (
                  <th key={h} className="px-4 py-2.5 text-left text-[10px] font-semibold text-text-muted uppercase tracking-widest">
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody className="divide-y divide-white/[0.04]">
              {enrichedThreads.map((t) => (
                <tr key={t.tid} className="hover:bg-white/[0.02] transition-colors">
                  <td className="px-4 py-2.5 font-mono text-text-muted">{t.tid}</td>
                  <td className="px-4 py-2.5 font-medium text-text-primary">{t.comm}</td>
                  <td className="px-4 py-2.5">
                    <div className="flex items-center gap-1.5">
                      <div className="w-12 h-1 rounded-full bg-white/[0.06] overflow-hidden">
                        <div className={clsx(
                          'h-full rounded-full',
                          t.cpu_pct > 30 ? 'bg-red-500' : t.cpu_pct > 10 ? 'bg-yellow-500' : 'bg-accent'
                        )} style={{ width: `${Math.min(100, t.cpu_pct)}%` }} />
                      </div>
                      <span className="font-mono font-semibold tabular-nums text-text-primary">
                        {t.cpu_pct.toFixed(1)}%
                      </span>
                    </div>
                  </td>
                  <td className="px-4 py-2.5 font-mono text-text-secondary tabular-nums">{t.user_pct.toFixed(1)}</td>
                  <td className="px-4 py-2.5 font-mono text-text-secondary tabular-nums">{t.sys_pct.toFixed(1)}</td>
                  <td className="px-4 py-2.5">
                    <span className={clsx(
                      'inline-flex items-center justify-center w-5 h-5 rounded text-[9px] font-bold',
                      t.state === 'R' ? 'bg-green-500/15 text-green-400' : 'bg-white/[0.04] text-text-muted'
                    )}>
                      {t.state}
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        )}
      </div>
    </div>
  );
}

function formatKb(kb: number): string {
  if (kb >= 1048576) return `${(kb / 1048576).toFixed(1)} GB`;
  if (kb >= 1024) return `${(kb / 1024).toFixed(0)} MB`;
  return `${kb} KB`;
}
