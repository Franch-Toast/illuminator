import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { ReplayEngine } from './replayEngine'

function createMockFile(content: string): File {
  return new File([content], 'test.ilr', { type: 'application/json' })
}

const SAMPLE_ILR = [
  '{"type":"header","version":1,"features":["cpu_utilization","cpu_processes"],"start_ts":1000,"end_ts":5000}',
  '{"ts":1000,"feature":"cpu_utilization","data":{"records":[{"labels":{"type":"cpu_total"},"fields":{"user_pct":10}}]}}',
  '{"ts":2000,"feature":"cpu_processes","data":{"records":[{"labels":{"type":"process","pid":"1"},"fields":{"cpu_total_pct":5}}]}}',
  '{"ts":3000,"feature":"cpu_utilization","data":{"records":[{"labels":{"type":"cpu_total"},"fields":{"user_pct":15}}]}}',
  '{"ts":4000,"feature":"cpu_utilization","data":{"records":[{"labels":{"type":"cpu_total"},"fields":{"user_pct":20}}]}}',
  '{"ts":5000,"feature":"cpu_processes","data":{"records":[{"labels":{"type":"process","pid":"1"},"fields":{"cpu_total_pct":8}}]}}',
].join('\n')

describe('ReplayEngine', () => {
  let engine: ReplayEngine

  beforeEach(() => {
    engine = new ReplayEngine()
  })

  afterEach(() => {
    engine.destroy()
  })

  it('should load file and parse metadata', async () => {
    const file = createMockFile(SAMPLE_ILR)
    const meta = await engine.loadFile(file)

    expect(meta.version).toBe(1)
    expect(meta.features).toContain('cpu_utilization')
    expect(meta.features).toContain('cpu_processes')
    expect(meta.startTs).toBe(1000)
    expect(meta.endTs).toBe(5000)
    expect(meta.frameCount).toBe(5)
  })

  it('should start in ready state after loading', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    expect(engine.getState()).toBe('ready')
    expect(engine.getProgress()).toBe(0)
  })

  it('should seek to position', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    engine.seek(3000)
    expect(engine.getCurrentTime()).toBe(3000)
    expect(engine.getProgress()).toBeCloseTo(0.5, 1)
  })

  it('should emit frames to subscribers', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    const received: unknown[] = []
    engine.subscribe('cpu_utilization', (batch) => {
      received.push(batch)
    })

    engine.seek(1500)
    expect(received.length).toBeGreaterThanOrEqual(1)
  })

  it('should clamp seek to valid range', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    engine.seek(10000)
    expect(engine.getCurrentTime()).toBe(5000)

    engine.seek(-1000)
    expect(engine.getCurrentTime()).toBe(1000)
  })

  it('should get available features', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    const features = engine.getAvailableFeatures()
    expect(features).toContain('cpu_utilization')
    expect(features).toContain('cpu_processes')
  })

  it('should set speed', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    engine.setSpeed(2)
    expect(engine.getState()).toBe('ready')
  })

  it('should track latest batch per feature', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    engine.seek(4500)

    const latest = engine.getLatest('cpu_utilization')
    expect(latest).not.toBeNull()
    expect(latest!.feature).toBe('cpu_utilization')
  })

  it('should notify state changes', async () => {
    const file = createMockFile(SAMPLE_ILR)
    const states: string[] = []
    engine.onStateChange(s => states.push(s))

    await engine.loadFile(file)
    expect(states).toContain('loading')
    expect(states).toContain('ready')
  })

  it('should handle malformed lines gracefully', async () => {
    const content = [
      '{"ts":1000,"feature":"test","data":{"val":1}}',
      'not valid json',
      '{"ts":2000,"feature":"test","data":{"val":2}}',
    ].join('\n')

    const file = createMockFile(content)
    const meta = await engine.loadFile(file)
    expect(meta.frameCount).toBe(2)
  })

  it('should clean up on destroy', async () => {
    const file = createMockFile(SAMPLE_ILR)
    await engine.loadFile(file)

    const cb = vi.fn()
    engine.subscribe('cpu_utilization', cb)
    engine.destroy()

    engine.seek(3000)
    expect(cb).not.toHaveBeenCalled()
  })
})
