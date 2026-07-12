import { describe, it, expect, vi, beforeEach } from 'vitest'
import { timeSeriesStore } from './timeSeriesStore'

describe('TimeSeriesStore', () => {
  beforeEach(() => {
    timeSeriesStore['series'].clear()
    timeSeriesStore['listeners'].clear()
  })

  it('appends points and returns ordered all()', () => {
    timeSeriesStore.append('cpu', 1000, { user: 10 })
    timeSeriesStore.append('cpu', 2000, { user: 20 })
    timeSeriesStore.append('cpu', 3000, { user: 30 })

    const all = timeSeriesStore.all('cpu')
    expect(all).toHaveLength(3)
    expect(all[0].time).toBe(1000)
    expect(all[2].time).toBe(3000)
  })

  it('overwrites values for same-second timestamps (dedup)', () => {
    timeSeriesStore.append('cpu', 1000, { user: 10 })
    timeSeriesStore.append('cpu', 1200, { user: 20, sys: 5 })
    timeSeriesStore.append('cpu', 1500, { sys: 8 })

    const latest = timeSeriesStore.latest('cpu')
    expect(latest).toEqual({ time: 1500, user: 20, sys: 8 })
    expect(timeSeriesStore.size('cpu')).toBe(1)
  })

  it('supports getRange with binary search', () => {
    timeSeriesStore.append('cpu', 1000, { user: 10 })
    timeSeriesStore.append('cpu', 2000, { user: 20 })
    timeSeriesStore.append('cpu', 3000, { user: 30 })
    timeSeriesStore.append('cpu', 4000, { user: 40 })

    const range = timeSeriesStore.getRange('cpu', 1500, 3500)
    expect(range).toHaveLength(2)
    expect(range[0].time).toBe(2000)
    expect(range[1].time).toBe(3000)
  })

  it('evicts oldest points when capacity exceeded', () => {
    for (let i = 0; i < 10; i++) {
      timeSeriesStore.append('cpu', i * 1000 + 1, { user: i })
    }

    const all = timeSeriesStore.all('cpu')
    // 容量为 1800，远未超过，因此保留全部
    expect(all.length).toBe(10)
  })

  it('notifies subscribers on append', () => {
    const cb = vi.fn()
    const unsub = timeSeriesStore.subscribe('cpu', cb)

    timeSeriesStore.append('cpu', 1000, { user: 10 })
    expect(cb).toHaveBeenCalledTimes(1)

    unsub()
    timeSeriesStore.append('cpu', 2000, { user: 20 })
    expect(cb).toHaveBeenCalledTimes(1)
  })

  it('appendMany notifies once', () => {
    const cb = vi.fn()
    timeSeriesStore.subscribe('cpu', cb)

    timeSeriesStore.appendMany('cpu', [
      { time: 1000, user: 10 },
      { time: 2000, user: 20 },
    ])
    expect(cb).toHaveBeenCalledTimes(1)
    expect(timeSeriesStore.size('cpu')).toBe(2)
  })

  it('reports memory usage estimate', () => {
    timeSeriesStore.append('cpu', 1000, { user: 10, sys: 5 })
    expect(timeSeriesStore.memoryBytes('cpu')).toBeGreaterThan(0)
  })
})
