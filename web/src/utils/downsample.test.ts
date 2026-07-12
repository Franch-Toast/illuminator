import { describe, it, expect } from 'vitest'
import { lttb, intervalSample, downsample } from './downsample'

describe('downsample', () => {
  it('returns original data when under threshold', () => {
    const data = [{ x: 0, y: 1 }, { x: 1, y: 2 }, { x: 2, y: 3 }]
    expect(downsample(data, { threshold: 10 })).toEqual(data)
  })

  it('intervalSample preserves first and last points', () => {
    const data = Array.from({ length: 100 }, (_, i) => ({ x: i, y: i }))
    const sampled = intervalSample(data, 10)
    expect(sampled[0]).toEqual(data[0])
    expect(sampled[sampled.length - 1]).toEqual(data[data.length - 1])
    expect(sampled.length).toBe(10)
  })

  it('lttb reduces size and keeps endpoints', () => {
    const data = Array.from({ length: 200 }, (_, i) => ({ x: i, y: Math.sin(i / 10) * 100 }))
    const sampled = lttb(data, 50)
    expect(sampled.length).toBe(50)
    expect(sampled[0]).toEqual(data[0])
    expect(sampled[sampled.length - 1]).toEqual(data[data.length - 1])
  })
})
