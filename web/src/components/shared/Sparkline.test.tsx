import { render } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import Sparkline from './Sparkline'

describe('Sparkline', () => {
  it('renders null when data has fewer than 2 points', () => {
    const { container } = render(<Sparkline data={[5]} color="#3b82f6" />)
    expect(container.firstChild).toBeNull()
  })

  it('renders an SVG with polyline for valid data', () => {
    const { container } = render(<Sparkline data={[0, 5, 10, 5]} color="#3b82f6" />)
    const svg = container.querySelector('svg')
    expect(svg).not.toBeNull()
    expect(svg!.getAttribute('width')).toBe('100')
    expect(svg!.getAttribute('height')).toBe('20')
    const polyline = container.querySelector('polyline')
    expect(polyline).not.toBeNull()
    expect(polyline!.getAttribute('stroke')).toBe('#3b82f6')
  })

  it('respects custom width and height', () => {
    const { container } = render(<Sparkline data={[1, 2, 3]} color="#000" width={200} height={40} />)
    const svg = container.querySelector('svg')
    expect(svg!.getAttribute('width')).toBe('200')
    expect(svg!.getAttribute('height')).toBe('40')
  })
})
