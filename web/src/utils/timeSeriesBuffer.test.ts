import { describe, it, expect, vi, afterEach } from 'vitest'
import { TimeSeriesBuffer } from './timeSeriesBuffer'

describe('TimeSeriesBuffer', () => {
  afterEach(() => { vi.restoreAllMocks() })

  it('stores pushed items', () => {
    const buf = new TimeSeriesBuffer<{ timestamp: number; v: number }>(60)
    buf.push({ timestamp: Date.now(), v: 1 })
    buf.push({ timestamp: Date.now(), v: 2 })
    expect(buf.length).toBe(2)
    expect(buf.getAll().map(i => i.v)).toEqual([1, 2])
  })

  it('pushMany adds multiple items', () => {
    const buf = new TimeSeriesBuffer<{ timestamp: number }>(60)
    const now = Date.now()
    buf.pushMany([{ timestamp: now }, { timestamp: now + 1 }, { timestamp: now + 2 }])
    expect(buf.length).toBe(3)
  })

  it('evicts items older than windowSec', () => {
    vi.spyOn(Date, 'now').mockReturnValue(100_000)
    const buf = new TimeSeriesBuffer<{ timestamp: number; v: string }>(10)
    buf.push({ timestamp: 85_000, v: 'old' })
    buf.push({ timestamp: 95_000, v: 'new' })
    expect(buf.length).toBe(1)
    expect(buf.getAll()[0].v).toBe('new')
  })

  it('getLast returns the last N items', () => {
    const buf = new TimeSeriesBuffer<{ timestamp: number; v: number }>(600)
    const now = Date.now()
    for (let i = 0; i < 10; i++) {
      buf.push({ timestamp: now + i * 100, v: i })
    }
    const last3 = buf.getLast(3)
    expect(last3.map(i => i.v)).toEqual([7, 8, 9])
  })

  it('clear empties the buffer', () => {
    const buf = new TimeSeriesBuffer<{ timestamp: number }>(60)
    buf.push({ timestamp: Date.now() })
    buf.clear()
    expect(buf.length).toBe(0)
  })

  it('binary search eviction handles edge case of all-expired', () => {
    vi.spyOn(Date, 'now').mockReturnValue(200_000)
    const buf = new TimeSeriesBuffer<{ timestamp: number }>(5)
    buf.pushMany([
      { timestamp: 100_000 },
      { timestamp: 110_000 },
      { timestamp: 120_000 },
    ])
    expect(buf.length).toBe(0)
  })
})
