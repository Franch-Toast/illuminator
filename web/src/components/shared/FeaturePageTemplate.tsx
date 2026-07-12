import { useState, type ReactNode } from 'react'
import { colors } from '../../styles/theme'
import { useFeatureHealth, type FeatureHealthInfo } from '../../hooks/useFeatureHealth'
import SubTabBar from '../SubTabBar'
import FeatureHealthBadge from '../FeatureHealthBadge'
import StateIndicator from './StateIndicator'
import ControlBar, { type DriverState } from './ControlBar'
import ErrorBoundary from './ErrorBoundary'

export interface TabConfig {
  key: string
  label: string
  icon?: ReactNode
}

interface FeaturePageTemplateProps {
  /** Feature 标识名（如 "cpu_profiler"） */
  featureName: string
  /** 显示名（如 "CPU Profiler"） */
  displayName: string
  /** 简短描述 */
  description?: string
  /** 子标签配置 */
  tabs?: TabConfig[]
  /** 自定义控制区（追加到默认控制栏后） */
  controls?: ReactNode
  /** 内容区（支持 render prop 接收 activeTab） */
  children: ReactNode | ((activeTab: string) => ReactNode)
  /** 是否显示控制栏（默认 true） */
  showControlBar?: boolean
  /** 是否显示状态栏（默认 true） */
  showStatusBar?: boolean
}

/** 将 FeatureHealthInfo 映射为 DriverState */
function healthToDriverState(health: FeatureHealthInfo): DriverState {
  if (health.status === 'error' || health.errors > 0 || health.state === 'error') {
    return 'error'
  }
  if (health.state === 'active') return 'active'
  if (health.state === 'paused') return 'paused'
  return 'inactive'
}

function formatUptime(ms: number): string {
  const sec = Math.round(ms / 1000)
  if (sec < 60) return `${sec}s`
  if (sec < 3600) return `${Math.floor(sec / 60)}m ${sec % 60}s`
  return `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`
}

/**
 * FeaturePageTemplate — Feature 页面统一容器组件
 *
 * 提供标准化的页面布局：
 *   ┌───────────────────────────────────────────┐
 *   │ [displayName]  [StateIndicator] [ControlBar] │ ← 标题栏
 *   ├───────────────────────────────────────────┤
 *   │ [Tab1] [Tab2] [Tab3]                          │ ← 子标签（可选）
 *   ├───────────────────────────────────────────┤
 *   │ {children}                                    │ ← 内容区
 *   ├───────────────────────────────────────────┤
 *   │ [HealthBadge] [Stats] [RecordingStatus]       │ ← 状态栏
 *   └───────────────────────────────────────────┘
 *
 * 内置 useFeatureHealth 获取状态、ErrorBoundary 包裹 children。
 */
export default function FeaturePageTemplate({
  featureName,
  displayName,
  description,
  tabs,
  controls,
  children,
  showControlBar = true,
  showStatusBar = true,
}: FeaturePageTemplateProps) {
  const health = useFeatureHealth(featureName)
  const driverState = healthToDriverState(health)
  const [activeTab, setActiveTab] = useState(tabs?.[0]?.key ?? '')

  const renderContent = (): ReactNode => {
    if (typeof children === 'function') {
      return (children as (tab: string) => ReactNode)(activeTab)
    }
    return children
  }

  return (
    <div style={{
      padding: 24, display: 'flex', flexDirection: 'column',
      minHeight: '100%', gap: 0,
    }}>
      {/* ── 标题栏 ── */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 12,
        padding: '0 0 12px', flexWrap: 'wrap',
      }}>
        <h2 style={{
          margin: 0, fontSize: 20, color: colors.textPrimary, fontWeight: 600,
        }}>
          {displayName}
        </h2>
        <StateIndicator state={driverState} />
        {description && (
          <span style={{ fontSize: 12, color: colors.textMuted }}>{description}</span>
        )}
        <div style={{ flex: 1 }} />
        {showControlBar && (
          <ControlBar featureName={featureName} state={driverState} />
        )}
        {controls}
      </div>

      {/* ── 子标签（可选） ── */}
      {tabs && tabs.length > 0 && (
        <SubTabBar
          tabs={tabs.map(t => ({ id: t.key, label: t.label }))}
          active={activeTab}
          onChange={setActiveTab}
        />
      )}

      {/* ── 内容区 ── */}
      <div style={{ flex: 1 }}>
        <ErrorBoundary>
          {renderContent()}
        </ErrorBoundary>
      </div>

      {/* ── 状态栏 ── */}
      {showStatusBar && (
        <div style={{
          display: 'flex', alignItems: 'center', gap: 16,
          padding: '12px 0 0', marginTop: 16,
          borderTop: `1px solid ${colors.cardBorder}`,
          flexWrap: 'wrap',
        }}>
          <FeatureHealthBadge featureName={featureName} showLabel />
          <span style={{ fontSize: 11, color: colors.textMuted }}>
            Uptime: {formatUptime(health.uptime_ms)}
          </span>
          <span style={{ fontSize: 11, color: colors.textMuted }}>
            Batches: {health.batches_processed.toLocaleString()}
          </span>
          <span style={{ fontSize: 11, color: colors.textMuted }}>
            Records: {health.records_processed.toLocaleString()}
          </span>
          {health.errors > 0 && (
            <span style={{ fontSize: 11, color: colors.danger }}>
              Errors: {health.errors}
            </span>
          )}
          {health.buffer_full_rate > 0 && (
            <span style={{
              fontSize: 11,
              color: health.buffer_full_rate > 5 ? colors.danger : health.buffer_full_rate > 1 ? colors.warnText : colors.textMuted,
            }}>
              BPF Overflow: {health.buffer_full_rate.toFixed(2)}%
            </span>
          )}
        </div>
      )}
    </div>
  )
}
