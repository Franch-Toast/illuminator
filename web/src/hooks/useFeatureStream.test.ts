import { describe, it, expect, beforeEach } from 'vitest'
import { TimeSeriesBuffer } from './useFeatureStream'

interface TestPoint {
  timestamp: number
  value: number
}

describe('TimeSeriesBuffer', () => {
  let buffer: TimeSeriesBuffer<TestPoint>

  beforeEach(() => {
    buffer = new TimeSeriesBuffer<TestPoint>(10)
  })

  it('should push and retrieve items', () => {
    buffer.push({ timestamp: Date.now(), value: 1 })
    buffer.push({ timestamp: Date.now(), value: 2 })
    expect(buffer.length).toBe(2)
    expect(buffer.getAll()).toHaveLength(2)
  })

  it('should evict items beyond window', () => {
    const now = Date.now()
    buffer.push({ timestamp: now - 15000, value: 1 })
    buffer.push({ timestamp: now - 12000, value: 2 })
    buffer.push({ timestamp: now - 5000, value: 3 })
    buffer.push({ timestamp: now, value: 4 })
    expect(buffer.length).toBe(2)
    expect(buffer.getAll()[0].value).toBe(3)
  })

  it('should handle pushMany', () => {
    const now = Date.now()
    buffer.pushMany([
      { timestamp: now - 1000, value: 1 },
      { timestamp: now, value: 2 },
    ])
    expect(buffer.length).toBe(2)
  })

  it('should getLast(n) correctly', () => {
    const now = Date.now()
    for (let i = 0; i < 5; i++) {
      buffer.push({ timestamp: now - (4 - i) * 1000, value: i })
    }
    const last3 = buffer.getLast(3)
    expect(last3).toHaveLength(3)
    expect(last3[0].value).toBe(2)
    expect(last3[2].value).toBe(4)
  })

  it('should clear properly', () => {
    buffer.push({ timestamp: Date.now(), value: 1 })
    buffer.clear()
    expect(buffer.length).toBe(0)
    expect(buffer.getAll()).toEqual([])
  })

  it('should handle empty buffer', () => {
    expect(buffer.length).toBe(0)
    expect(buffer.getAll()).toEqual([])
    expect(buffer.getLast(5)).toEqual([])
  })

  it('should handle binary search eviction with all stale data', () => {
    const stale = Date.now() - 20000
    buffer.pushMany([
      { timestamp: stale, value: 1 },
      { timestamp: stale + 1000, value: 2 },
      { timestamp: stale + 2000, value: 3 },
    ])
    expect(buffer.length).toBe(0)
  })

  it('should respect window size in seconds', () => {
    const smallBuffer = new TimeSeriesBuffer<TestPoint>(2)
    const now = Date.now()
    smallBuffer.push({ timestamp: now - 3000, value: 1 })
    smallBuffer.push({ timestamp: now - 1000, value: 2 })
    smallBuffer.push({ timestamp: now, value: 3 })
    expect(smallBuffer.length).toBe(2)
    expect(smallBuffer.getAll()[0].value).toBe(2)
  })
})
