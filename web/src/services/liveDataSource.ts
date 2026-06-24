/**
 * ============================================================================
 * LiveDataSource — 前端数据源（Link Chain 架构）
 * ============================================================================
 *
 * 【架构定位】
 * LiveDataSource 是 Illuminator 前端的核心数据层，实现了"链路链（Link Chain）"
 * 架构。它通过两个 Link（WsLink 和 HttpLink）从后端获取数据，并向上层组件
 * 提供统一的数据订阅接口。
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │                     React 组件层                                 │
 *   │  CpuPanel  │  MemoryPanel  │  IoPanel  │  ProfilerPanel  │ ... │
 *   └──────┬─────┴──────┬────────┴───────────┴────────┬──────────────┘
 *          │            │                              │
 *          │  subscribe("cpu", cb)  subscribe("mem", cb)
 *          │            │                              │
 *   ┌──────▼────────────▼──────────────────────────────▼──────────────┐
 *   │                   LiveDataSource (DataSource 接口)                │
 *   │                                                                  │
 *   │  ┌─────────────────────────────────────────────────────────┐    │
 *   │  │  subs: Map<feature, FeatureSubscription>                 │    │
 *   │  │    "cpu_utilization" → { callbacks: Set<cb>, latest }   │    │
 *   │  │    "cpu_profiler"    → { callbacks: Set<cb>, latest }   │    │
 *   │  │    ...                                                   │    │
 *   │  └─────────────────────────────────────────────────────────┘    │
 *   │                                                                  │
 *   │  ┌──────────────────┐         ┌──────────────────┐             │
 *   │  │  WsLink (主链路)  │         │ HttpLink (备链路)│             │
 *   │  │  WebSocket 连接   │ ─fail─→ │ HTTP 轮询        │             │
 *   │  │  实时推送         │         │ 定时拉取          │             │
 *   │  └────────┬─────────┘         └────────┬─────────┘             │
 *   └───────────┼────────────────────────────┼────────────────────────┘
 *               │                            │
 *   ┌───────────▼────────────────────────────▼────────────────────────┐
 *   │                   Illuminator 后端                                │
 *   │  WebSocket 广播 (ws://host:9527/ws/features)                     │
 *   │  HTTP API 拉取 (/api/v1/features/:name/collect)                  │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * 【双链路架构】
 *   WsLink（主链路）：
 *     - 通过 WebSocket 连接到后端，接收实时数据推送
 *     - 后端通过 WebSocketManager::BroadcastLoop 定期广播数据
 *     - 优势：实时性高，延迟低
 *     - 劣势：连接可能断开，需要备链路
 *
 *   HttpLink（备链路）：
 *     - 当 WebSocket 断开时自动激活
 *     - 通过 HTTP 定时轮询 /api/v1/features/:name/collect 获取数据
 *     - 使用 cursor 机制避免重复消费（/api/v1/features/:name/stream?cursor=N）
 *     - 优势：可靠性高，HTTP 总是可用
 *     - 劣势：延迟较高（取决于轮询间隔）
 *
 * 【页面可见性感知】
 *   当用户切换标签页或最小化窗口时（visibilityState === 'hidden'）：
 *     - HttpLink 停止轮询（节省带宽和电池）
 *     - WebSocket 连接保持（继续接收实时数据）
 *   当用户切回页面时（visibilityState === 'visible'）：
 *     - 如果 WebSocket 已断开，重新连接
 *     - 激活 HttpLink 恢复轮询（作为备链路）
 *
 * 【使用示例】
 *   const ds = new LiveDataSource();
 *
 *   // 订阅 Feature 数据
 *   const unsubscribe = ds.subscribe("cpu_utilization", (batch) => {
 *     console.log("New CPU data:", batch);
 *   });
 *
 *   // 获取最新数据
 *   const latest = ds.getLatest("cpu_utilization");
 *
 *   // 取消订阅
 *   unsubscribe();
 *
 *   // 销毁（释放所有资源）
 *   ds.destroy();
 */

import type { DataBatch, DataCallback, DataSource, ConnectionStatus, DataSourceEvents } from './dataSource'
import { WsLink } from './links/WsLink'
import { HttpLink } from './links/HttpLink'

/**
 * FeatureSubscription — 单个 Feature 的订阅状态
 *
 * 每个被订阅的 Feature 对应一个 FeatureSubscription 实例。
 */
type FeatureSubscription = {
  /** 回调函数集合（一个 Feature 可以被多个组件订阅） */
  callbacks: Set<DataCallback>
  /** 最新数据缓存（用于 getLatest() 快速返回） */
  latest: DataBatch | null
}

/**
 * LiveDataSource — 链路链（Link Chain）数据源
 *
 * 核心职责：
 *   1. 管理 WebSocket 和 HTTP 两条数据链路
 *   2. 在 WebSocket 断开时自动切换到 HTTP 轮询
 *   3. 感知页面可见性状态，优化资源使用
 *   4. 向上层组件提供统一的数据订阅/取消订阅接口
 *
 * 数据流：
 *   WsLink (onData) ──→ handleData() ──→ 遍历回调集合 ──→ 组件重新渲染
 *   HttpLink (onData) ─┘
 *
 * 状态管理：
 *   - connected:    WebSocket 已连接，HttpLink 停止
 *   - disconnected: WebSocket 已断开，HttpLink 激活（如果页面可见）
 *   - 状态变更通过 events.onConnectionChange 回调通知上层
 */
export class LiveDataSource implements DataSource {
  /** Feature 订阅表：key = Feature 名称，value = 订阅状态 */
  private subs = new Map<string, FeatureSubscription>()

  /** WebSocket 链路（主链路，实时推送） */
  private wsLink: WsLink

  /** HTTP 链路（备链路，定时轮询） */
  private httpLink: HttpLink

  /** 当前连接状态 */
  private status: ConnectionStatus = 'disconnected'

  /** 事件回调（连接状态变更等） */
  private events: DataSourceEvents

  /** 页面是否可见 */
  private visible = true

  /** 页面可见性事件监听器 */
  private visibilityHandler: (() => void) | null = null

  /**
   * 构造 LiveDataSource
   *
   * 参数：
   *   events:         数据源事件回调（如连接状态变更）
   *   pollIntervalMs: HTTP 轮询间隔（毫秒，默认 1000ms）
   *                   轮询间隔越短，数据更新越实时，但后端压力越大
   */
  constructor(events: DataSourceEvents = {}, pollIntervalMs = 1000) {
    this.events = events

    /**
     * handleData — 统一的回调分发器
     *
     * 当 WsLink 或 HttpLink 收到新数据时，调用此函数将数据分发给
     * 所有订阅了该 Feature 的组件回调。
     *
     * 数据流：
     *   WsLink.onMessage → handleData(batch) → subs[feature].callbacks.forEach(cb => cb(batch))
     *   HttpLink.onResponse → handleData(batch) → 同上
     */
    const handleData = (batch: DataBatch) => {
      const sub = this.subs.get(batch.feature)
      if (sub) {
        sub.latest = batch  // 缓存最新数据
        // 通知所有订阅者
        for (const cb of sub.callbacks) cb(batch)
      }
    }

    /**
     * handleStatus — 连接状态变更处理器
     *
     * 当 WebSocket 连接状态变化时，自动切换主备链路：
     *   - connected:    停止 HttpLink 轮询（节省资源）
     *   - disconnected: 如果页面可见，激活 HttpLink 轮询（作为备链路）
     */
    const handleStatus = (s: ConnectionStatus) => {
      if (this.status === s) return  // 状态未变化，忽略
      this.status = s
      this.events.onConnectionChange?.(s)

      if (s === 'connected') {
        // WebSocket 已连接，停止 HTTP 轮询
        this.httpLink.disconnect()
      } else if (s === 'disconnected' && this.visible) {
        // WebSocket 断开，激活 HTTP 轮询（仅当页面可见时）
        this.activateHttpFallback()
      }
    }

    // 创建两条链路
    // WsLink: 连接到 ws://host:port/ws/features
    this.wsLink = new WsLink(this.getWsUrl(), handleData, handleStatus)
    // HttpLink: 轮询 /api/v1/features/:name/collect
    this.httpLink = new HttpLink(handleData, pollIntervalMs)

    // 设置页面可见性监听
    this.setupVisibility()

    // 启动 WebSocket 连接（主链路优先）
    this.wsLink.connect()
  }

  /**
   * subscribe — 订阅 Feature 数据
   *
   * 当组件需要显示某个 Feature 的实时数据时，调用此方法订阅。
   * 返回一个取消订阅函数，组件卸载时调用。
   *
   * 参数：
   *   feature: Feature 名称（如 "cpu_utilization"）
   *   cb:      数据回调函数（收到新数据时调用）
   *
   * 返回：
   *   取消订阅函数（调用后停止接收该 Feature 的数据）
   *
   * 示例：
   *   useEffect(() => {
   *     const unsubscribe = ds.subscribe("cpu_utilization", (batch) => {
   *       setCpuData(batch);
   *     });
   *     return unsubscribe;  // 组件卸载时自动取消订阅
   *   }, []);
   */
  subscribe(feature: string, cb: DataCallback): () => void {
    // 获取或创建 Feature 的订阅状态
    let sub = this.subs.get(feature)
    if (!sub) {
      sub = { callbacks: new Set(), latest: null }
      this.subs.set(feature, sub)
    }
    sub.callbacks.add(cb)

    // 根据连接状态选择链路
    if (this.wsLink.isConnected()) {
      // WebSocket 已连接 → 发送订阅消息到后端
      this.wsLink.subscribe(feature)
    } else {
      // WebSocket 未连接 → 使用 HTTP 轮询
      this.httpLink.connect()
      this.httpLink.subscribe(feature)
    }

    // 返回取消订阅函数
    return () => {
      sub!.callbacks.delete(cb)
      // 如果没有订阅者了，从链路中取消订阅
      if (sub!.callbacks.size === 0) {
        this.wsLink.unsubscribe(feature)
        this.httpLink.unsubscribe(feature)
        this.subs.delete(feature)
      }
    }
  }

  /**
   * getLatest — 获取 Feature 的最新数据（同步返回）
   *
   * 从缓存中返回最新数据，不触发网络请求。
   * 用于组件初始化时显示已有数据，避免空白状态。
   *
   * 参数：
   *   feature: Feature 名称
   *
   * 返回：
   *   最新数据批次，如果从未收到数据则返回 null
   */
  getLatest(feature: string): DataBatch | null {
    return this.subs.get(feature)?.latest ?? null
  }

  /**
   * getAvailableFeatures — 获取当前已订阅的 Feature 列表
   */
  getAvailableFeatures(): string[] {
    return Array.from(this.subs.keys())
  }

  /**
   * getStatus — 获取当前连接状态
   */
  getStatus(): ConnectionStatus {
    return this.status
  }

  /**
   * destroy — 销毁数据源（释放所有资源）
   *
   * 在应用退出或页面卸载时调用，确保：
   *   - 移除页面可见性监听器
   *   - 断开 WebSocket 连接
   *   - 停止 HTTP 轮询
   *   - 清空订阅表
   */
  destroy(): void {
    if (this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler)
    }
    this.wsLink.disconnect()
    this.httpLink.disconnect()
    this.subs.clear()
  }

  /**
   * getWsUrl — 构建 WebSocket 连接 URL
   *
   * 根据当前页面的协议和主机自动构建 WebSocket URL。
   *   - https:// → wss://（安全连接）
   *   - http://  → ws://（非安全连接）
   *
   * 返回：
   *   WebSocket URL（如 "ws://localhost:9527/ws/features"）
   */
  private getWsUrl(): string {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const host = window.location.host
    return `${protocol}//${host}/ws/features`
  }

  /**
   * setupVisibility — 设置页面可见性监听
   *
   * 监听 document.visibilitychange 事件，优化资源使用：
   *   - 页面隐藏时：停止 HTTP 轮询（节省带宽和电池）
   *   - 页面显示时：如果 WebSocket 已断开，重新连接并激活 HTTP 轮询
   *
   * 为什么页面隐藏时不停止 WebSocket？
   *   - WebSocket 是被动接收数据，不主动发送请求
   *   - 保持连接可以避免页面恢复时的重连延迟
   *   - 后端会继续广播，但数据会在订阅缓存中更新
   */
  private setupVisibility() {
    this.visibilityHandler = () => {
      const wasVisible = this.visible
      this.visible = document.visibilityState === 'visible'

      if (!wasVisible && this.visible) {
        // 从隐藏变为可见
        if (!this.wsLink.isConnected()) {
          // WebSocket 已断开，重新连接
          this.wsLink.connect()
          this.activateHttpFallback()
        }
      } else if (wasVisible && !this.visible) {
        // 从可见变为隐藏：停止 HTTP 轮询
        this.httpLink.disconnect()
      }
    }
    document.addEventListener('visibilitychange', this.visibilityHandler)
  }

  /**
   * activateHttpFallback — 激活 HTTP 备链路
   *
   * 当 WebSocket 断开时，为所有已订阅的 Feature 启动 HTTP 轮询。
   * 遍历 subs 中的所有 Feature，逐个调用 httpLink.subscribe。
   */
  private activateHttpFallback() {
    this.httpLink.connect()
    for (const feature of this.subs.keys()) {
      this.httpLink.subscribe(feature)
    }
  }
}