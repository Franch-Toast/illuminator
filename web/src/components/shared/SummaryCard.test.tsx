import { render, screen } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import SummaryCard from './SummaryCard'

describe('SummaryCard', () => {
  it('renders label and value', () => {
    render(<SummaryCard label="CPU" value="42%" />)
    expect(screen.getByText('CPU')).toBeDefined()
    expect(screen.getByText('42%')).toBeDefined()
  })

  it('applies custom color prop', () => {
    const { container } = render(<SummaryCard label="Used" value="80%" color="#ef4444" />)
    const html = container.innerHTML
    expect(html).toContain('rgb(239, 68, 68)')
  })

  it('renders without error when no color prop', () => {
    render(<SummaryCard label="Free" value="20%" />)
    expect(screen.getByText('20%')).toBeDefined()
  })
})
