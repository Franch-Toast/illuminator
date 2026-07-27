/**
 * DataBus — 数据总线
 *
 * 连接 SseLink 和各 Zustand Store：
 * - 将 time_series 消息路由到 TimeSeriesStore
 * - 更新 ConnectionStore 的连接指标
 * - 更新 FeatureStore 的 feature 状态和 sparkData
 */

import { sseLink } from './sse-link';
import type { SseMessage } from './sse-link';
import { useTimeSeriesStore } from '../stores/timeseries-store';
import { useConnectionStore } from '../stores/connection-store';

let initialized = false;
let messageCount = 0;
let startTime = 0;

export function initDataBus() {
  if (initialized) return;
  initialized = true;
  startTime = Date.now();

  sseLink.onMessage((msg: SseMessage) => {
    messageCount++;

    if (msg.modelType === 'time_series') {
      useTimeSeriesStore.getState().ingestMessage(msg);
    }

    if (messageCount % 5 === 0) {
      const elapsed = (Date.now() - startTime) / 1000;
      useConnectionStore.getState().setMessagesPerSec(
        Math.round(messageCount / Math.max(elapsed, 1))
      );
    }
  });


  sseLink.connect(['cpu_utilization', 'process_cpu']);
}

export function stopDataBus() {
  sseLink.disconnect();
  initialized = false;
  messageCount = 0;
}
