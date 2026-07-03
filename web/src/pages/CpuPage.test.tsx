import { render, screen } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { describe, it, expect, vi } from 'vitest'

vi.mock('../services/apiClient', () => ({
  api: {
    featureStart: vi.fn().mockResolvedValue({ status: 'ok' }),
    featureStop: vi.fn().mockResolvedValue({ status: 'ok' }),
    features: vi.fn().mockResolvedValue({ features: [] }),
  },
}))

vi.mock('../hooks/useProcessDetail', () => ({
  useProcessDetail: () => ({ timeline: [], threads: [], processGone: false }),
}))

vi.mock('../hooks/useCpuData', () => ({
  useCpuUtilization: () => ({ areaData: [], coreData: [], summary: null, clear: vi.fn() }),
  useCpuProcesses: () => ({ processes: [], clear: vi.fn() }),
}))

vi.mock('../hooks/useDataSource', () => ({
  getDataSource: () => ({
    subscribe: () => () => {},
    getLatest: () => null,
  }),
}))

vi.mock('../components/FeatureHealthBadge', () => ({
  default: () => <span data-testid="health-badge" />,
}))

vi.mock('../components/charts/ProfileSnapshot', () => ({
  default: () => <div data-testid="profile-snapshot" />,
}))

vi.mock('../components/charts/ProcessCpuTimeline', () => ({
  default: () => <div data-testid="cpu-timeline" />,
}))

vi.mock('../components/charts/ThreadBreakdown', () => ({
  default: () => <div data-testid="thread-breakdown" />,
}))

vi.mock('../components/charts/StackedAreaChart', () => ({
  default: () => <div data-testid="stacked-area" />,
}))

vi.mock('../components/charts/CoreHeatmap', () => ({
  default: () => <div data-testid="core-heatmap" />,
}))

vi.mock('../components/charts/SummaryCards', () => ({
  default: () => <div data-testid="summary-cards" />,
}))

vi.mock('../components/charts/ProcessTable', () => ({
  default: () => <div data-testid="process-table" />,
}))

import CpuPage from './CpuPage'

describe('CpuPage', () => {
  it('renders the CPU page with tabs', () => {
    render(<MemoryRouter><CpuPage /></MemoryRouter>)
    expect(screen.getByText('CPU')).toBeDefined()
  })

  it('renders health badge', () => {
    render(<MemoryRouter><CpuPage /></MemoryRouter>)
    expect(screen.getByTestId('health-badge')).toBeDefined()
  })
})
