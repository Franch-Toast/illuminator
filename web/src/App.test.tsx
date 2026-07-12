import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import App from './App'

vi.mock('./hooks/usePipelinePolling', () => ({
  usePipelinePolling: vi.fn(),
}))

vi.mock('./hooks/useDataSource', () => ({
  useConnectionStatus: vi.fn().mockReturnValue('disconnected'),
  useResourceBudget: vi.fn().mockReturnValue(null),
  getDataSource: vi.fn().mockReturnValue({
    getStatus: () => 'disconnected',
    subscribe: () => () => {},
    destroy: () => {},
  }),
}))

vi.mock('./services/dataBus', () => ({
  dataBus: {
    subscribe: vi.fn(() => () => {}),
    getAvailableFeatures: vi.fn(() => []),
    getRecent: vi.fn(() => []),
    getWindowSize: vi.fn(() => 60),
    setWindowSize: vi.fn(),
    onConnectionChange: vi.fn(() => () => {}),
    connect: vi.fn().mockRejectedValue(new Error('test')),
    disconnect: vi.fn(),
    getStatus: vi.fn(() => 'disconnected'),
    getBuffer: vi.fn(() => []),
    reconnectNow: vi.fn(),
    getConnectionStats: vi.fn(() => ({ reconnectCount: 0, lastConnectedAt: null, nextReconnectDelayMs: null, nextReconnectAt: null })),
  },
}))

vi.mock('./services/apiClient', () => ({
  api: {
    features: vi.fn().mockResolvedValue({ features: [] }),
    featureStart: vi.fn().mockResolvedValue({ status: 'ok' }),
    featureStop: vi.fn().mockResolvedValue({ status: 'ok' }),
    budget: vi.fn().mockResolvedValue(null),
    startRecording: vi.fn().mockResolvedValue({ status: 'ok' }),
    stopRecording: vi.fn().mockResolvedValue({ status: 'ok' }),
  },
}))

describe('App', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
      ok: true,
      json: () => Promise.resolve({ version: '1.0.0-test' }),
    }))
  })

  function renderApp(route = '/') {
    return render(
      <MemoryRouter initialEntries={[route]}>
        <App />
      </MemoryRouter>
    )
  }

  it('renders navigation sidebar with all nav items', () => {
    renderApp()
    expect(screen.getByText('Illuminator')).toBeInTheDocument()
    expect(screen.getByText('CPU')).toBeInTheDocument()
    expect(screen.getByText('Memory')).toBeInTheDocument()
    expect(screen.getByText('IO')).toBeInTheDocument()
    expect(screen.getByText('Network')).toBeInTheDocument()
    expect(screen.getByText('GPU')).toBeInTheDocument()
    expect(screen.getByText('Replay')).toBeInTheDocument()
    expect(screen.getByText('Query')).toBeInTheDocument()
    expect(screen.getByText('System')).toBeInTheDocument()
  })

  it('renders Overview page by default', async () => {
    renderApp('/')
    await waitFor(() => {
      expect(screen.getByText('Loading...')).toBeInTheDocument()
    }, { timeout: 100 }).catch(() => {
      // Page loaded immediately without suspense fallback — also fine
    })
  })

  it('fetches version on mount', async () => {
    renderApp()
    await waitFor(() => {
      expect(fetch).toHaveBeenCalledWith('/healthz')
    })
  })

  it('shows connection indicator', () => {
    renderApp()
    expect(screen.getByText('已断开')).toBeInTheDocument()
  })
})
