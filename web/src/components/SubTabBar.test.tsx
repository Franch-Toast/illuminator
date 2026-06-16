import { describe, it, expect, vi } from 'vitest'
import { render, screen, fireEvent } from '@testing-library/react'
import SubTabBar from './SubTabBar'

describe('SubTabBar', () => {
  const tabs = [
    { id: 'util', label: 'Utilization' },
    { id: 'procs', label: 'Processes' },
    { id: 'profile', label: 'Profiling' },
  ]

  it('renders all tabs', () => {
    render(<SubTabBar tabs={tabs} active="util" onChange={() => {}} />)

    expect(screen.getByText('Utilization')).toBeInTheDocument()
    expect(screen.getByText('Processes')).toBeInTheDocument()
    expect(screen.getByText('Profiling')).toBeInTheDocument()
  })

  it('highlights the active tab', () => {
    render(<SubTabBar tabs={tabs} active="procs" onChange={() => {}} />)

    const activeBtn = screen.getByText('Processes')
    expect(activeBtn).toHaveStyle({ fontWeight: 600 })
  })

  it('calls onChange when a tab is clicked', () => {
    const onChange = vi.fn()
    render(<SubTabBar tabs={tabs} active="util" onChange={onChange} />)

    fireEvent.click(screen.getByText('Profiling'))
    expect(onChange).toHaveBeenCalledWith('profile')
  })

  it('renders nothing for empty tabs array', () => {
    const { container } = render(<SubTabBar tabs={[]} active="" onChange={() => {}} />)
    expect(container.querySelector('button')).toBeNull()
  })
})
