import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen } from '@testing-library/react'
import ConnectionIndicator from './ConnectionIndicator'

vi.mock('../../hooks/useDataSource', () => ({
  useConnectionStatus: vi.fn(),
}))

import { useConnectionStatus } from '../../hooks/useDataSource'
const mockUseConnectionStatus = vi.mocked(useConnectionStatus)

describe('ConnectionIndicator', () => {
  beforeEach(() => {
    vi.clearAllMocks()
  })

  it('shows "SSE Connected" when status is connected', () => {
    mockUseConnectionStatus.mockReturnValue('connected')
    render(<ConnectionIndicator />)
    expect(screen.getByText('SSE Connected')).toBeInTheDocument()
  })

  it('shows "Connecting..." when status is connecting', () => {
    mockUseConnectionStatus.mockReturnValue('connecting')
    render(<ConnectionIndicator />)
    expect(screen.getByText('Connecting...')).toBeInTheDocument()
  })

  it('shows "Disconnected" when status is disconnected', () => {
    mockUseConnectionStatus.mockReturnValue('disconnected')
    render(<ConnectionIndicator />)
    expect(screen.getByText('Disconnected')).toBeInTheDocument()
  })

  it('has correct title tooltip for connected state', () => {
    mockUseConnectionStatus.mockReturnValue('connected')
    const { container } = render(<ConnectionIndicator />)
    const wrapper = container.firstChild as HTMLElement
    expect(wrapper.title).toContain('Real-time SSE connection active')
  })

  it('has correct title tooltip for disconnected state', () => {
    mockUseConnectionStatus.mockReturnValue('disconnected')
    const { container } = render(<ConnectionIndicator />)
    const wrapper = container.firstChild as HTMLElement
    expect(wrapper.title).toContain('SSE connection lost')
  })
})
