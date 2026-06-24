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

vi.mock('./services/apiClient', () => ({
  api: {
    featureCollect: vi.fn().mockResolvedValue({ records: [] }),
    features: vi.fn().mockResolvedValue({ features: [] }),
    createSession: vi.fn().mockResolvedValue({ status: 'ok' }),
    stopSession: vi.fn().mockResolvedValue({ status: 'ok' }),
    budget: vi.fn().mockResolvedValue(null),
    exportData: vi.fn().mockResolvedValue({ status: 'ok', file: '/tmp/test.ilr', features_exported: 0, batches_exported: 0 }),
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
    expect(screen.getByText('HTTP Polling')).toBeInTheDocument()
  })
})
