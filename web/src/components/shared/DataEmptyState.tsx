import type { ReactNode } from 'react'
import { colors } from '../../styles/theme'

interface DataEmptyStateProps {
  /** 是否为加载态 */
  loading?: boolean
  /** 自定义标题 */
  title?: string
  /** 描述文字 */
  description?: string
  /** 自定义图标 */
  icon?: ReactNode
  /** 容器高度 */
  height?: number
}

/**
 * DataEmptyState — 无数据时的占位显示
 *
 * 支持两种模式：
 *   - loading: 显示 "等待数据中..." 加载态
 *   - empty: 显示 "暂无数据" 空态
 */
export default function DataEmptyState({
  loading = false,
  title,
  description,
  icon,
  height = 200,
}: DataEmptyStateProps) {
  return (
    <div style={{
      height, display: 'flex', flexDirection: 'column',
      alignItems: 'center', justifyContent: 'center', gap: 8,
    }}>
      {loading ? (
        <>
          <span style={{
            display: 'inline-block', fontSize: 24, color: colors.accent,
            animation: 'data-empty-spin 1s linear infinite',
          }}>⟳</span>
          <span style={{ fontSize: 13, color: colors.textMuted }}>
            {title || '等待数据中...'}
          </span>
          {description && (
            <span style={{ fontSize: 11, color: colors.textDimmed }}>{description}</span>
          )}
        </>
      ) : (
        <>
          {icon || (
            <span style={{ fontSize: 32, opacity: 0.3, color: colors.textMuted }}>📭</span>
          )}
          <span style={{ fontSize: 13, color: colors.textMuted }}>
            {title || '暂无数据'}
          </span>
          {description && (
            <span style={{ fontSize: 11, color: colors.textDimmed }}>{description}</span>
          )}
        </>
      )}
      <style>{`
        @keyframes data-empty-spin {
          from { transform: rotate(0deg); }
          to { transform: rotate(360deg); }
        }
      `}</style>
    </div>
  )
}
