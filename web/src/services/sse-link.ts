/**
 * SseLink — SSE 连接管理器
 *
 * 负责与后端建立 Server-Sent Events 连接，自动重连（指数退避），
 * 并将接收到的数据分发给 DataBus。
 *
 * 当后端不可用时，自动切换到 mock 模式生成模拟数据。
 */

import { useConnectionStore } from '../stores/connection-store';

export interface SseMessage {
  feature: string;
  modelType: 'time_series' | 'profile' | 'trace';
  timestamp: number;
  seq: number;
  metrics?: Array<{
    labels: Record<string, string>;
    fields: Record<string, number | string>;
    timestamp: number;
  }>;
  records?: Array<Record<string, unknown>>;
}

type MessageHandler = (msg: SseMessage) => void;

const API_BASE = '/api/v1/events';
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 30000;

export class SseLink {
  private es: EventSource | null = null;
  private handlers: MessageHandler[] = [];
  private reconnectMs = RECONNECT_MIN_MS;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private msgCount = 0;
  private startTime = 0;
  private lastMsgTime = 0;
  private mockTimer: ReturnType<typeof setInterval> | null = null;
  private useMock = false;
  private subscriptionId: string | null = null;

  onMessage(handler: MessageHandler) {
    this.handlers.push(handler);
    return () => {
      this.handlers = this.handlers.filter((h) => h !== handler);
    };
  }

  async connect(features: string[] = []) {
    this.disconnect();
    this.startTime = Date.now();
    this.msgCount = 0;

    try {
      const subId = await this.subscribe(features);
      if (!subId) {
        this.startMockMode();
        return;
      }
      this.subscriptionId = subId;
      this.openSseStream(subId, features);
    } catch {
      console.info('[SseLink] Backend unreachable, switching to mock mode');
      this.startMockMode();
    }
  }

  private async subscribe(features: string[]): Promise<string | null> {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 3000);

    try {
      const res = await fetch(`${API_BASE}/subscribe`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ features }),
        signal: controller.signal,
      });
      clearTimeout(timeout);

      if (!res.ok) return null;
      const data = await res.json();
      return data.subscription_id ?? null;
    } catch {
      clearTimeout(timeout);
      return null;
    }
  }

  private openSseStream(subId: string, features: string[]) {
    const url = `${API_BASE}/${subId}`;
    this.es = new EventSource(url);

    this.es.onopen = () => {
      this.stopMockMode();
      this.reconnectMs = RECONNECT_MIN_MS;
      useConnectionStore.getState().setConnected(true);
    };

    this.es.addEventListener('data', (event) => {
      try {
        const msg: SseMessage = JSON.parse(event.data);
        this.msgCount++;
        this.lastMsgTime = Date.now();
        this.dispatch(msg);
      } catch { /* 忽略格式错误的消息 */ }
    });

    this.es.onmessage = (event) => {
      try {
        const msg: SseMessage = JSON.parse(event.data);
        this.msgCount++;
        this.lastMsgTime = Date.now();
        this.dispatch(msg);
      } catch { /* 忽略格式错误的消息 */ }
    };

    this.es.onerror = () => {
      useConnectionStore.getState().setConnected(false);
      if (!this.useMock) {
        this.scheduleReconnect(features);
      }
    };
  }

  disconnect() {
    if (this.es) {
      this.es.close();
      this.es = null;
    }
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.stopMockMode();
    useConnectionStore.getState().setConnected(false);
  }

  /** 启动 mock 模式（后端不可用时自动生成模拟数据） */
  startMockMode() {
    if (this.mockTimer) return;
    this.useMock = true;
    useConnectionStore.getState().setConnected(true);

    this.startTime = Date.now();
    this.msgCount = 0;

    this.mockTimer = setInterval(() => {
      const now = Date.now();
      this.dispatch(generateMockCpuData(now));
      this.dispatch(generateMockProcessData(now));
      this.msgCount += 2;
      this.lastMsgTime = now;

      const elapsed = (now - this.startTime) / 1000 || 1;
      useConnectionStore.getState().setMessagesPerSec(
        Math.round(this.msgCount / Math.max(elapsed, 1))
      );
    }, 1000);
  }

  stopMockMode() {
    if (this.mockTimer) {
      clearInterval(this.mockTimer);
      this.mockTimer = null;
    }
    this.useMock = false;
  }

  isMockMode() {
    return this.useMock;
  }

  private dispatch(msg: SseMessage) {
    for (const h of this.handlers) {
      try { h(msg); } catch { /* 隔离处理异常 */ }
    }
  }

  private reconnectAttempts = 0;

  private scheduleReconnect(features: string[]) {
    if (this.reconnectTimer) return;
    this.reconnectAttempts++;

    if (this.reconnectAttempts >= 3) {
      console.info(`[SseLink] ${this.reconnectAttempts} attempts failed, switching to mock mode`);
      this.reconnectAttempts = 0;
      this.startMockMode();
      return;
    }

    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect(features);
    }, this.reconnectMs);
    this.reconnectMs = Math.min(this.reconnectMs * 2, RECONNECT_MAX_MS);
  }
}

// ---- Mock 数据生成 ----

let mockBusy = 45;
let mockUser = 30;
let mockSys = 12;

function generateMockCpuData(now: number): SseMessage {
  mockUser = clamp(mockUser + (Math.random() - 0.48) * 8, 5, 70);
  mockSys = clamp(mockSys + (Math.random() - 0.48) * 5, 2, 40);
  const iowait = clamp(3 + (Math.random() - 0.5) * 4, 0, 20);
  mockBusy = mockUser + mockSys + iowait;
  const idle = 100 - mockBusy;

  return {
    feature: 'cpu_utilization',
    modelType: 'time_series',
    timestamp: now,
    seq: 0,
    metrics: [{
      labels: { type: 'system_total' },
      fields: {
        user_pct: round2(mockUser),
        system_pct: round2(mockSys),
        idle_pct: round2(idle),
        iowait_pct: round2(iowait),
        busy_pct: round2(mockBusy),
        irq_pct: round2(Math.random() * 2),
        softirq_pct: round2(Math.random() * 3),
        steal_pct: 0,
        nice_pct: round2(Math.random()),
        ctxt_per_sec: Math.round(8000 + Math.random() * 4000),
        intr_per_sec: Math.round(5000 + Math.random() * 3000),
        procs_running: Math.round(2 + Math.random() * 6),
        procs_blocked: Math.round(Math.random() * 2),
        load_1m: round2(1.5 + Math.random() * 2),
        load_5m: round2(1.8 + Math.random() * 1.5),
        load_15m: round2(2.0 + Math.random()),
      },
      timestamp: now,
    }],
  };
}

const mockProcs = [
  { pid: '1234', comm: 'nginx',     baseCpu: 25 },
  { pid: '5678', comm: 'node',      baseCpu: 15 },
  { pid: '9012', comm: 'postgres',  baseCpu: 10 },
  { pid: '3456', comm: 'redis',     baseCpu: 6 },
  { pid: '7890', comm: 'python3',   baseCpu: 4 },
  { pid: '2345', comm: 'java',      baseCpu: 3 },
  { pid: '6789', comm: 'go-svc',    baseCpu: 2 },
  { pid: '1357', comm: 'systemd',   baseCpu: 0.5 },
];

function generateMockProcessData(now: number): SseMessage {
  return {
    feature: 'process_cpu',
    modelType: 'time_series',
    timestamp: now,
    seq: 0,
    metrics: mockProcs.map((p) => {
      const total = clamp(p.baseCpu + (Math.random() - 0.5) * p.baseCpu * 0.4, 0, 100);
      const userRatio = 0.6 + Math.random() * 0.3;
      return {
        labels: { type: 'process', pid: p.pid, comm: p.comm },
        fields: {
          cpu_total_pct: round2(total),
          cpu_user_pct: round2(total * userRatio),
          cpu_sys_pct: round2(total * (1 - userRatio)),
          state: Math.random() > 0.7 ? 'R' : 'S',
          num_threads: Math.round(2 + Math.random() * 20),
          rss_kb: Math.round(50000 + Math.random() * 500000),
          vsize_kb: Math.round(100000 + Math.random() * 2000000),
        },
        timestamp: now,
      };
    }),
  };
}

function clamp(v: number, min: number, max: number) {
  return Math.max(min, Math.min(max, v));
}

function round2(v: number) {
  return Math.round(v * 100) / 100;
}

export const sseLink = new SseLink();
