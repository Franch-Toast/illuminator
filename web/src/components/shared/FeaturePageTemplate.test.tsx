import { type ReactElement } from 'react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, fireEvent } from '@testing-library/react'

// Mock useFeatureHealth before importing components that depend on it
vi.mock('../../hooks/useFeatureHealth', () => ({
  useFeatureHealth: vi.fn(() => ({
    status: 'active',
    state: 'active',
    uptime_ms: 60000,
    batches_processed: 42,
    records_processed: 1024,
    errors: 0,
    lastUpdatedAt: Date.now(),
    bpf_total_events: 0,
    bpf_buffer_full: 0,
    bpf_dropped: 0,
    bpf_filtered: 0,
    buffer_full_rate: 0,
  })),
}))

vi.mock('../../services/apiClient', () => ({
  api: {
    featureStart: vi.fn().mockResolvedValue({ status: 'ok' }),
    featureStop: vi.fn().mockResolvedValue({ status: 'ok' }),
    featurePause: vi.fn().mockResolvedValue({ status: 'ok' }),
    featureResume: vi.fn().mockResolvedValue({ status: 'ok' }),
    featureStats: vi.fn().mockResolvedValue({}),
  },
}))

vi.mock('../../hooks/useDataSource', () => ({
  getDataSource: () => ({
    subscribe: () => () => {},
    getLatest: () => null,
  }),
}))

vi.mock('../FeatureHealthBadge', () => ({
  default: ({ featureName }: { featureName: string }) => (
    <span data-testid="health-badge" data-feature={featureName} />
  ),
}))

vi.mock('../RecordingControls', () => ({
  default: () => <span data-testid="recording-controls" />,
}))

import FeaturePageTemplate from './FeaturePageTemplate'
import { useFeatureHealth } from '../../hooks/useFeatureHealth'

const mockHealth = vi.mocked(useFeatureHealth)

function setHealthState(overrides: Record<string, unknown>) {
  mockHealth.mockReturnValue({
    status: 'active',
    state: 'active',
    uptime_ms: 0,
    batches_processed: 0,
    records_processed: 0,
    errors: 0,
    lastUpdatedAt: Date.now(),
    bpf_total_events: 0,
    bpf_buffer_full: 0,
    bpf_dropped: 0,
    bpf_filtered: 0,
    buffer_full_rate: 0,
    ...overrides,
  } as ReturnType<typeof useFeatureHealth>)
}

describe('FeaturePageTemplate', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    setHealthState({})
  })

  it('renders the display name', () => {
    render(
      <FeaturePageTemplate featureName="cpu_profiler" displayName="CPU Profiler">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('CPU Profiler')).toBeDefined()
  })

  it('renders the description when provided', () => {
    render(
      <FeaturePageTemplate featureName="cpu_profiler" displayName="CPU" description="性能分析">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('性能分析')).toBeDefined()
  })

  it('renders health badge in status bar', () => {
    render(
      <FeaturePageTemplate featureName="cpu_utilization" displayName="CPU">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    const badge = screen.getByTestId('health-badge')
    expect(badge).toBeDefined()
    expect(badge.getAttribute('data-feature')).toBe('cpu_utilization')
  })

  it('renders uptime, batches and records in status bar', () => {
    setHealthState({ uptime_ms: 65000, batches_processed: 10, records_processed: 256 })
    render(
      <FeaturePageTemplate featureName="test_feature" displayName="Test">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText(/Uptime:/)).toBeDefined()
    expect(screen.getByText(/1m 5s/)).toBeDefined()
    expect(screen.getByText(/Batches: 10/)).toBeDefined()
    expect(screen.getByText(/Records: 256/)).toBeDefined()
  })

  it('renders error count when errors > 0', () => {
    setHealthState({ errors: 3 })
    render(
      <FeaturePageTemplate featureName="test_feature" displayName="Test">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText(/Errors: 3/)).toBeDefined()
  })

  it('renders tabs and switches between them', () => {
    render(
      <FeaturePageTemplate
        featureName="test"
        displayName="Test"
        tabs={[
          { key: 'overview', label: '总览' },
          { key: 'detail', label: '详情' },
        ]}
      >
        {(activeTab: string) => (
          <div>
            <span data-testid="active-tab">{activeTab}</span>
          </div>
        )}
      </FeaturePageTemplate>,
    )
    // Default tab is the first one
    expect(screen.getByTestId('active-tab').textContent).toBe('overview')

    // Click second tab
    fireEvent.click(screen.getByText('详情'))
    expect(screen.getByTestId('active-tab').textContent).toBe('detail')
  })

  it('renders children as ReactNode (not render prop)', () => {
    render(
      <FeaturePageTemplate featureName="test" displayName="Test">
        <div data-testid="static-content">static</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByTestId('static-content')).toBeDefined()
  })

  it('renders without tabs', () => {
    render(
      <FeaturePageTemplate featureName="test" displayName="Simple">
        <div>simple content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('Simple')).toBeDefined()
    expect(screen.getByText('simple content')).toBeDefined()
  })

  it('hides control bar when showControlBar is false', () => {
    render(
      <FeaturePageTemplate featureName="test" displayName="Test" showControlBar={false}>
        <div>content</div>
      </FeaturePageTemplate>,
    )
    // No start/pause/stop buttons should be present
    expect(screen.queryByText('启动')).toBeNull()
    expect(screen.queryByText('暂停')).toBeNull()
    expect(screen.queryByText('停止')).toBeNull()
  })

  it('hides status bar when showStatusBar is false', () => {
    render(
      <FeaturePageTemplate featureName="test" displayName="Test" showStatusBar={false}>
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.queryByTestId('health-badge')).toBeNull()
    expect(screen.queryByText(/Uptime:/)).toBeNull()
  })

  it('renders custom controls prop', () => {
    render(
      <FeaturePageTemplate
        featureName="test"
        displayName="Test"
        controls={<button data-testid="custom-btn">Custom</button>}
      >
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByTestId('custom-btn')).toBeDefined()
  })

  it('shows StateIndicator with active state', () => {
    setHealthState({ status: 'active', state: 'active' })
    render(
      <FeaturePageTemplate featureName="test" displayName="Test">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('运行中')).toBeDefined()
  })

  it('shows StateIndicator with error state when health has errors', () => {
    setHealthState({ errors: 1 })
    render(
      <FeaturePageTemplate featureName="test" displayName="Test">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('异常')).toBeDefined()
  })

  it('shows StateIndicator with paused state', () => {
    setHealthState({ state: 'paused' })
    render(
      <FeaturePageTemplate featureName="test" displayName="Test">
        <div>content</div>
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('已暂停')).toBeDefined()
  })

  it('catches child errors via ErrorBoundary', () => {
    function BrokenChild(): ReactElement {
      throw new Error('child render error')
    }
    // Suppress console.error for this test
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {})
    render(
      <FeaturePageTemplate featureName="test" displayName="Test">
        <BrokenChild />
      </FeaturePageTemplate>,
    )
    expect(screen.getByText('页面渲染出错')).toBeDefined()
    expect(screen.getByText('重试')).toBeDefined()
    spy.mockRestore()
  })
})
