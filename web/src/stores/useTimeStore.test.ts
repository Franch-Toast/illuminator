import { describe, it, expect, beforeEach } from 'vitest'
import { useTimeStore } from './useTimeStore'

describe('useTimeStore', () => {
  beforeEach(() => {
    useTimeStore.setState({
      mode: 'live',
      windowMs: 60_000,
      cursor: null,
    })
  })

  it('initializes in live mode with 60s window', () => {
    const state = useTimeStore.getState()
    expect(state.mode).toBe('live')
    expect(state.windowMs).toBe(60_000)
  })

  it('togglePause switches between live and paused', () => {
    useTimeStore.getState().togglePause()
    expect(useTimeStore.getState().mode).toBe('paused')
    useTimeStore.getState().togglePause()
    expect(useTimeStore.getState().mode).toBe('live')
  })

  it('setWindowMs updates windowMs and refreshes range', () => {
    useTimeStore.getState().setWindowMs(300_000)
    const state = useTimeStore.getState()
    expect(state.windowMs).toBe(300_000)
    expect(state.range.end - state.range.start).toBeCloseTo(300_000, -2)
  })

  it('setRange pauses time', () => {
    const range = { start: 1000, end: 2000 }
    useTimeStore.getState().setRange(range)
    expect(useTimeStore.getState().mode).toBe('paused')
    expect(useTimeStore.getState().range).toEqual(range)
  })

  it('tick updates range only in live mode', () => {
    const before = useTimeStore.getState().range.end
    useTimeStore.getState().tick()
    const after = useTimeStore.getState().range.end
    expect(after).toBeGreaterThanOrEqual(before)

    useTimeStore.getState().setMode('paused')
    const paused = useTimeStore.getState().range.end
    useTimeStore.getState().tick()
    expect(useTimeStore.getState().range.end).toBe(paused)
  })
})
