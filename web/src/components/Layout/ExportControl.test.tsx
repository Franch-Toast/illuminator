import { render, screen, fireEvent, waitFor } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { MemoryRouter } from 'react-router-dom'
import ExportControl from './ExportControl'

const mockNavigate = vi.fn()
vi.mock('react-router-dom', async () => {
  const actual = await vi.importActual('react-router-dom')
  return { ...actual, useNavigate: () => mockNavigate }
})

vi.mock('../../services/apiClient', () => ({
  api: {
    exportData: vi.fn().mockResolvedValue({
      file: 'export_2026.ilr',
      features_exported: 3,
      batches_exported: 42,
    }),
  },
}))

describe('ExportControl', () => {
  beforeEach(() => { mockNavigate.mockClear() })

  const renderControl = () => render(
    <MemoryRouter><ExportControl /></MemoryRouter>
  )

  it('renders export button and lookback select', () => {
    renderControl()
    expect(screen.getByText('Export .ilr')).toBeDefined()
    expect(screen.getByRole('combobox')).toBeDefined()
  })

  it('shows Replay and Download buttons after successful export', async () => {
    renderControl()
    fireEvent.click(screen.getByText('Export .ilr'))
    await waitFor(() => {
      expect(screen.getByText('Replay')).toBeDefined()
      expect(screen.getByText('Download')).toBeDefined()
    })
  })

  it('navigates to replay page on Replay click', async () => {
    renderControl()
    fireEvent.click(screen.getByText('Export .ilr'))
    await waitFor(() => screen.getByText('Replay'))
    fireEvent.click(screen.getByText('Replay'))
    expect(mockNavigate).toHaveBeenCalledWith('/replay?file=export_2026.ilr')
  })

  it('shows feature and batch count after export', async () => {
    renderControl()
    fireEvent.click(screen.getByText('Export .ilr'))
    await waitFor(() => {
      expect(screen.getByText('3 features, 42 batches')).toBeDefined()
    })
  })
})
