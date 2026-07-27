/**
 * TimeSeriesStore — 时间序列数据环形缓冲
 *
 * 按 feature + label key 存储实时时间序列数据。
 * 使用环形缓冲区（固定 maxPoints），超出时丢弃最旧的点。
 * 提供给 ECharts 组件消费的 {ts, value}[] 格式。
 */

import { create } from 'zustand';
import type { SseMessage } from '../services/sse-link';

export interface DataPoint {
  ts: number;
  value: number;
}

interface SeriesBuffer {
  data: DataPoint[];
}

interface TimeSeriesState {
  series: Map<string, SeriesBuffer>;
  processComms: Map<string, string>;
  maxPoints: number;

  getSeries: (key: string) => DataPoint[];
  getSeriesKeys: (featurePrefix: string) => string[];
  ingestMessage: (msg: SseMessage) => void;
  clear: () => void;
}

/**
 * series key 格式：`{feature}/{label_type}/{field_name}`
 * 示例：`cpu_utilization/system_total/user_pct`
 *       `cpu_utilization/cpu_core:cpu0/busy_pct`
 *       `process_cpu/process:1234/cpu_total_pct`
 */
function buildSeriesKey(feature: string, labels: Record<string, string>, field: string): string {
  const type = labels['type'] || 'unknown';

  if (type === 'process' && labels['pid']) {
    return `${feature}/process:${labels['pid']}/${field}`;
  }

  if (type === 'cpu_core' && labels['cpu']) {
    return `${feature}/cpu_core:${labels['cpu']}/${field}`;
  }

  const qualifiers = Object.entries(labels)
    .filter(([k]) => k !== 'type')
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, v]) => v)
    .join(':');
  const labelPart = qualifiers ? `${type}:${qualifiers}` : type;
  return `${feature}/${labelPart}/${field}`;
}

export const useTimeSeriesStore = create<TimeSeriesState>((set, get) => ({
  series: new Map(),
  processComms: new Map(),
  maxPoints: 300,

  getSeries(key: string) {
    return get().series.get(key)?.data ?? [];
  },

  getSeriesKeys(featurePrefix: string) {
    const keys: string[] = [];
    const prefix = featurePrefix + '/';
    for (const k of get().series.keys()) {
      if (k.startsWith(prefix)) keys.push(k);
    }
    return keys;
  },

  ingestMessage(msg: SseMessage) {
    if (!msg.metrics || msg.metrics.length === 0) return;

    set((state) => {
      const newMap = new Map(state.series);
      const newComms = new Map(state.processComms);
      const max = state.maxPoints;

      for (const metric of msg.metrics!) {
        const ts = metric.timestamp || msg.timestamp;
        const labels = metric.labels;

        if (labels['type'] === 'process' && labels['pid'] && labels['comm']) {
          newComms.set(labels['pid'], labels['comm']);
        }

        for (const [field, value] of Object.entries(metric.fields)) {
          if (typeof value !== 'number') continue;

          const key = buildSeriesKey(msg.feature, labels, field);
          let buf = newMap.get(key);
          if (!buf) {
            buf = { data: [] };
            newMap.set(key, buf);
          }

          const newData = buf.data.length >= max
            ? [...buf.data.slice(1), { ts, value }]
            : [...buf.data, { ts, value }];
          newMap.set(key, { data: newData });
        }
      }

      return { series: newMap, processComms: newComms };
    });
  },

  clear() {
    set({ series: new Map() });
  },
}));
