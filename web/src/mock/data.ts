export type FeatureStatus = 'active' | 'paused' | 'inactive' | 'error';
export type FeatureTier = 'monitoring' | 'profiling' | 'tracing';
export type ModelType = 'time_series' | 'profile' | 'trace';

export interface Feature {
  name: string;
  displayName: string;
  status: FeatureStatus;
  tier: FeatureTier;
  modelType: ModelType;
  interval?: string;
  summary: string;
  sparkData: number[];
  config: Record<string, unknown>;
}

export interface GoldenSignal {
  label: string;
  value: number;
  unit: string;
  trend: 'up' | 'down' | 'stable';
  sparkData: number[];
  thresholdWarn?: number;
  thresholdCrit?: number;
}

export interface FlameFrame {
  name: string;
  value: number;
  selfValue: number;
  children: FlameFrame[];
}

function genSpark(n: number, base: number, amp: number): number[] {
  const arr: number[] = [];
  let v = base;
  for (let i = 0; i < n; i++) {
    v += (Math.random() - 0.48) * amp;
    v = Math.max(base * 0.3, Math.min(base * 1.8, v));
    arr.push(Math.round(v * 100) / 100);
  }
  return arr;
}

function genTimeSeries(points: number, base: number, amp: number) {
  const now = Date.now();
  return Array.from({ length: points }, (_, i) => ({
    ts: now - (points - i) * 1000,
    value: base + Math.sin(i * 0.15) * amp + (Math.random() - 0.5) * amp * 0.4,
  }));
}

export const goldenSignals: GoldenSignal[] = [
  { label: 'Latency', value: 2.3, unit: 'ms', trend: 'down', sparkData: genSpark(24, 2.5, 0.6), thresholdWarn: 5, thresholdCrit: 10 },
  { label: 'Throughput', value: 1247, unit: 'req/s', trend: 'up', sparkData: genSpark(24, 1200, 150) },
  { label: 'Error Rate', value: 0.012, unit: '%', trend: 'stable', sparkData: genSpark(24, 0.01, 0.005), thresholdWarn: 1, thresholdCrit: 5 },
  { label: 'Saturation', value: 45, unit: '%', trend: 'up', sparkData: genSpark(24, 42, 8), thresholdWarn: 70, thresholdCrit: 90 },
];

export const features: Feature[] = [
  { name: 'cpu_utilization', displayName: 'CPU Utilization', status: 'active', tier: 'monitoring', modelType: 'time_series', interval: '1s', summary: '78% avg', sparkData: genSpark(20, 75, 12), config: { sample_rate: 1000, per_core: true } },
  { name: 'cpu_profiler', displayName: 'CPU Profiler', status: 'active', tier: 'profiling', modelType: 'profile', interval: '10s', summary: '1.2k samples', sparkData: genSpark(20, 1200, 200), config: { frequency: 99, stack_depth: 64 } },
  { name: 'offcpu_profiler', displayName: 'Off-CPU Profiler', status: 'paused', tier: 'profiling', modelType: 'profile', summary: 'paused', sparkData: genSpark(20, 0, 0.5), config: { min_block_us: 100 } },
  { name: 'memory_usage', displayName: 'Memory Usage', status: 'active', tier: 'monitoring', modelType: 'time_series', interval: '2s', summary: '4.2 GiB', sparkData: genSpark(20, 4200, 300), config: { track_rss: true, track_swap: true } },
  { name: 'disk_io', displayName: 'Disk I/O', status: 'active', tier: 'monitoring', modelType: 'time_series', interval: '1s', summary: '120 MB/s', sparkData: genSpark(20, 120, 30), config: { devices: ['sda', 'nvme0n1'] } },
  { name: 'network_io', displayName: 'Network I/O', status: 'active', tier: 'monitoring', modelType: 'time_series', interval: '1s', summary: '850 Mbps', sparkData: genSpark(20, 850, 100), config: { interfaces: ['eth0'] } },
  { name: 'scheduler_latency', displayName: 'Scheduler Latency', status: 'inactive', tier: 'tracing', modelType: 'trace', summary: 'inactive', sparkData: genSpark(20, 0, 0.3), config: { threshold_us: 50 } },
];

function makeFrame(name: string, value: number, children: FlameFrame[] = []): FlameFrame {
  const childSum = children.reduce((s, c) => s + c.value, 0);
  return { name, value, selfValue: value - childSum, children };
}

export const flameData: FlameFrame = makeFrame('root', 1000, [
  makeFrame('main', 800, [
    makeFrame('process_request', 500, [
      makeFrame('parse_input', 100),
      makeFrame('validate', 80),
      makeFrame('execute_query', 300, [
        makeFrame('db_lookup', 180, [makeFrame('index_scan', 120), makeFrame('fetch_rows', 60)]),
        makeFrame('serialize_result', 100),
      ]),
    ]),
    makeFrame('handle_response', 250, [
      makeFrame('compress', 150, [makeFrame('gzip_deflate', 120)]),
      makeFrame('send_data', 80),
    ]),
  ]),
  makeFrame('gc_collect', 120, [makeFrame('mark_sweep', 80), makeFrame('compact', 40)]),
  makeFrame('idle_loop', 80),
]);

export const cpuTimeSeriesData = {
  user: genTimeSeries(120, 45, 15),
  system: genTimeSeries(120, 18, 8),
  iowait: genTimeSeries(120, 5, 3),
  idle: genTimeSeries(120, 32, 10),
};

export const processTableData = [
  { pid: 1234, comm: 'nginx', cpu: 45.2, mem: 128, threads: 8, state: 'R' as const },
  { pid: 5678, comm: 'node', cpu: 22.1, mem: 256, threads: 12, state: 'S' as const },
  { pid: 9012, comm: 'postgres', cpu: 15.8, mem: 512, threads: 4, state: 'R' as const },
  { pid: 3456, comm: 'redis', cpu: 8.3, mem: 64, threads: 4, state: 'S' as const },
  { pid: 7890, comm: 'python3', cpu: 5.4, mem: 384, threads: 2, state: 'R' as const },
  { pid: 2345, comm: 'java', cpu: 3.7, mem: 1024, threads: 32, state: 'S' as const },
  { pid: 6789, comm: 'go-service', cpu: 2.1, mem: 96, threads: 16, state: 'R' as const },
  { pid: 1357, comm: 'systemd', cpu: 0.5, mem: 12, threads: 1, state: 'S' as const },
];
