import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, fireEvent, waitFor } from '@testing-library/react'

const mockApi = vi.hoisted(() => ({
  featureStart: vi.fn().mockResolvedValue({ status: 'ok' }),
  featureStop: vi.fn().mockResolvedValue({ status: 'ok' }),
  featurePause: vi.fn().mockResolvedValue({ status: 'ok' }),
  featureResume: vi.fn().mockResolvedValue({ status: 'ok' }),
}))

vi.mock('../../services/apiClient', () => ({
  api: mockApi,
}))

vi.mock('../RecordingControls', () => ({
  default: ({ featureName }: { featureName: string }) => (
    <span data-testid="recording-controls" data-feature={featureName} />
  ),
}))

import ControlBar from './ControlBar'

describe('ControlBar', () => {
  beforeEach(() => {
    vi.clearAllMocks()
  })

  // ── State: inactive ──
  it('shows start button when state is inactive', () => {
    render(<ControlBar featureName="test_feature" state="inactive" />)
    expect(screen.getByText('启动')).toBeDefined()
    expect(screen.queryByText('暂停')).toBeNull()
    expect(screen.queryByText('停止')).toBeNull()
  })

  it('calls api.featureStart on start click', async () => {
    render(<ControlBar featureName="test_feature" state="inactive" />)
    fireEvent.click(screen.getByText('启动'))
    await waitFor(() => {
      expect(mockApi.featureStart).toHaveBeenCalledWith('test_feature')
    })
  })

  // ── State: active ──
  it('shows pause, stop and recording when state is active', () => {
    render(<ControlBar featureName="test_feature" state="active" />)
    expect(screen.getByText('暂停')).toBeDefined()
    expect(screen.getByText('停止')).toBeDefined()
    expect(screen.getByTestId('recording-controls')).toBeDefined()
  })

  it('calls api.featurePause on pause click', async () => {
    render(<ControlBar featureName="cpu_profiler" state="active" />)
    fireEvent.click(screen.getByText('暂停'))
    await waitFor(() => {
      expect(mockApi.featurePause).toHaveBeenCalledWith('cpu_profiler')
    })
  })

  it('calls api.featureStop on stop click', async () => {
    render(<ControlBar featureName="cpu_profiler" state="active" />)
    fireEvent.click(screen.getByText('停止'))
    await waitFor(() => {
      expect(mockApi.featureStop).toHaveBeenCalledWith('cpu_profiler')
    })
  })

  it('hides recording controls when showRecording is false', () => {
    render(<ControlBar featureName="test" state="active" showRecording={false} />)
    expect(screen.queryByTestId('recording-controls')).toBeNull()
  })

  // ── State: paused ──
  it('shows resume and stop when state is paused', () => {
    render(<ControlBar featureName="test_feature" state="paused" />)
    expect(screen.getByText('恢复')).toBeDefined()
    expect(screen.getByText('停止')).toBeDefined()
    expect(screen.queryByText('启动')).toBeNull()
    expect(screen.queryByText('暂停')).toBeNull()
  })

  it('calls api.featureResume on resume click', async () => {
    render(<ControlBar featureName="net_tracer" state="paused" />)
    fireEvent.click(screen.getByText('恢复'))
    await waitFor(() => {
      expect(mockApi.featureResume).toHaveBeenCalledWith('net_tracer')
    })
  })

  // ── State: error ──
  it('shows retry button when state is error', () => {
    render(<ControlBar featureName="test_feature" state="error" />)
    expect(screen.getByText('重试启动')).toBeDefined()
  })

  it('calls api.featureStart on retry click', async () => {
    render(<ControlBar featureName="io_monitor" state="error" />)
    fireEvent.click(screen.getByText('重试启动'))
    await waitFor(() => {
      expect(mockApi.featureStart).toHaveBeenCalledWith('io_monitor')
    })
  })

  // ── Custom handlers ──
  it('uses custom onStart handler when provided', async () => {
    const onStart = vi.fn().mockResolvedValue(undefined)
    render(<ControlBar featureName="test" state="inactive" onStart={onStart} />)
    fireEvent.click(screen.getByText('启动'))
    await waitFor(() => {
      expect(onStart).toHaveBeenCalledTimes(1)
      expect(mockApi.featureStart).not.toHaveBeenCalled()
    })
  })

  it('uses custom onStop handler when provided', async () => {
    const onStop = vi.fn().mockResolvedValue(undefined)
    render(<ControlBar featureName="test" state="active" onStop={onStop} />)
    fireEvent.click(screen.getByText('停止'))
    await waitFor(() => {
      expect(onStop).toHaveBeenCalledTimes(1)
      expect(mockApi.featureStop).not.toHaveBeenCalled()
    })
  })

  // ── Error handling ──
  it('shows error message when API call fails', async () => {
    mockApi.featureStart.mockRejectedValueOnce(new Error('Connection refused'))
    render(<ControlBar featureName="test_feature" state="inactive" />)
    fireEvent.click(screen.getByText('启动'))
    await waitFor(() => {
      expect(screen.getByText('Connection refused')).toBeDefined()
    })
  })

  it('disables buttons while busy', async () => {
    let resolveStart: (v: { status: string }) => void
    mockApi.featureStart.mockReturnValueOnce(new Promise(r => { resolveStart = r }))
    render(<ControlBar featureName="test" state="inactive" />)
    const btn = screen.getByText('启动') as HTMLButtonElement
    fireEvent.click(btn)
    // While the promise is pending, the button should be disabled
    await waitFor(() => {
      expect(btn.disabled).toBe(true)
    })
    resolveStart!({ status: 'ok' })
    await waitFor(() => {
      expect(btn.disabled).toBe(false)
    })
  })

  // ── Extra prop ──
  it('renders extra nodes', () => {
    render(
      <ControlBar
        featureName="test"
        state="active"
        extra={<button data-testid="extra-btn">Extra</button>}
      />,
    )
    expect(screen.getByTestId('extra-btn')).toBeDefined()
  })
})
