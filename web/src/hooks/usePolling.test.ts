import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { act } from '@testing-library/react'
import { useTimeStore } from '../stores/useTimeStore'

describe('usePolling integration', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    useTimeStore.setState({ mode: 'live', windowMs: 60_000 })
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('useTimeStore.tick updates range in live mode', () => {
    const { getState } = useTimeStore
    const oldEnd = getState().range.end
    act(() => {
      getState().tick()
    })
    expect(getState().range.end).toBeGreaterThanOrEqual(oldEnd)
  })

  it('paused mode stops range updates', () => {
    const { getState } = useTimeStore
    getState().setMode('paused')
    const frozenEnd = getState().range.end
    getState().tick()
    expect(getState().range.end).toBe(frozenEnd)
  })
})
