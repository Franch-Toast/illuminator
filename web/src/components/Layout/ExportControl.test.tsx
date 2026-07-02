import { render, screen, fireEvent } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import ExportControl from './ExportControl'

vi.mock('../../services/dataBus', () => ({
  dataBus: {
    getAvailableFeatures: vi.fn(() => ['cpu_utilization', 'memory']),
    getRecent: vi.fn(() => [
      { feature: 'cpu_utilization', timestamp: 1000, data: { value: 1 } },
      { feature: 'cpu_utilization', timestamp: 2000, data: { value: 2 } },
    ]),
  },
}))

describe('ExportControl', () => {
  beforeEach(() => { vi.clearAllMocks() })

  const renderControl = () => render(<ExportControl />)

  it('renders export button and lookback select', () => {
    renderControl()
    expect(screen.getByText('Export')).toBeDefined()
    expect(screen.getByRole('combobox')).toBeDefined()
  })

  it('shows Saved! after click', () => {
    const createObjectURL = vi.fn(() => 'blob:url')
    const revokeObjectURL = vi.fn()
    globalThis.URL.createObjectURL = createObjectURL
    globalThis.URL.revokeObjectURL = revokeObjectURL

    renderControl()
    fireEvent.click(screen.getByText('Export'))
    expect(screen.getByText('Saved!')).toBeDefined()
    expect(createObjectURL).toHaveBeenCalled()
  })

  it('has lookback options', () => {
    renderControl()
    const select = screen.getByRole('combobox') as HTMLSelectElement
    expect(select.options.length).toBe(3)
  })
})
