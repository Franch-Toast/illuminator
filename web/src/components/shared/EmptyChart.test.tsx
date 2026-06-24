import { render, screen } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import EmptyChart from './EmptyChart'

describe('EmptyChart', () => {
  it('renders the message text', () => {
    render(<EmptyChart message="No data available" />)
    expect(screen.getByText('No data available')).toBeDefined()
  })

  it('uses default height of 100px', () => {
    const { container } = render(<EmptyChart message="Loading..." />)
    const el = container.firstChild as HTMLElement
    expect(el.style.height).toBe('100px')
  })

  it('applies custom height', () => {
    const { container } = render(<EmptyChart message="Empty" height={200} />)
    const el = container.firstChild as HTMLElement
    expect(el.style.height).toBe('200px')
  })
})
