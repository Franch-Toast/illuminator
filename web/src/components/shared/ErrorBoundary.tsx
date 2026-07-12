import React from 'react'
import { colors } from '../../styles/theme'

interface ErrorBoundaryProps {
  children: React.ReactNode
  fallback?: React.ReactNode
}

interface ErrorBoundaryState {
  hasError: boolean
  error: Error | null
}

/**
 * ErrorBoundary — React Error Boundary 实现
 *
 * 捕获子组件树中的渲染错误，显示友好的错误信息与重试按钮，
 * 同时将错误信息记录到 console。
 */
export default class ErrorBoundary extends React.Component<ErrorBoundaryProps, ErrorBoundaryState> {
  constructor(props: ErrorBoundaryProps) {
    super(props)
    this.state = { hasError: false, error: null }
  }

  static getDerivedStateFromError(error: Error): ErrorBoundaryState {
    return { hasError: true, error }
  }

  componentDidCatch(error: Error, errorInfo: React.ErrorInfo): void {
    console.error('[ErrorBoundary]', error, errorInfo.componentStack)
  }

  handleRetry = () => {
    this.setState({ hasError: false, error: null })
  }

  render() {
    if (this.state.hasError) {
      if (this.props.fallback) return this.props.fallback

      return (
        <div style={{
          padding: 32, textAlign: 'center',
          background: colors.dangerBg, border: `1px solid ${colors.dangerBorder}`,
          borderRadius: 8, margin: 16,
        }}>
          <div style={{ fontSize: 28, marginBottom: 8 }}>⚠</div>
          <h3 style={{ margin: '0 0 8px', fontSize: 15, color: colors.danger }}>
            页面渲染出错
          </h3>
          <p style={{ margin: '0 0 4px', fontSize: 12, color: colors.textSecondary }}>
            {this.state.error?.message || '发生未知错误'}
          </p>
          <button
            onClick={this.handleRetry}
            style={{
              marginTop: 12, padding: '6px 18px', fontSize: 13,
              background: colors.blue, color: '#fff', border: 'none',
              borderRadius: 6, cursor: 'pointer', fontWeight: 500,
            }}
          >
            重试
          </button>
        </div>
      )
    }

    return this.props.children
  }
}
