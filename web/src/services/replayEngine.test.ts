import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { ReplayEngine } from './replayEngine'

function createMockFile(content: string): File {
  return new File([content], 'test.ilr', { type: 'application/json' })
}

function createStreamableFile(content: string): File {
  const file = createMockFile(content)
  const encoder = new TextEncoder()
  const bytes = encoder.encode(content)
  // Simulate chunked reading (split into 2 chunks)
  const mid = Math.floor(bytes.length / 2)
  const chunk1 = bytes.slice(0, mid)
  const chunk2 = bytes.slice(mid)

  Object.defineProperty(file, 'stream', {
    value: () => new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(chunk1)
        controller.enqueue(chunk2)
        controller.close()
      }
    }),
  })
  return file
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

describe('ReplayEngine - Streaming', () => {
  let engine: ReplayEngine

  beforeEach(() => {
    engine = new ReplayEngine()
  })

  afterEach(() => {
    engine.destroy()
  })

  it('should load file via stream() when available', async () => {
    const file = createStreamableFile(SAMPLE_ILR)
    const meta = await engine.loadFile(file)

    expect(meta.features).toContain('cpu_utilization')
    expect(meta.features).toContain('cpu_processes')
    expect(meta.frameCount).toBe(5)
    expect(engine.getState()).toBe('ready')
  })

  it('should report progress during streaming load', async () => {
    const file = createStreamableFile(SAMPLE_ILR)
    const progressValues: number[] = []

    await engine.loadFile(file, (pct) => {
      progressValues.push(pct)
    })

    expect(progressValues.length).toBeGreaterThan(0)
    expect(progressValues[progressValues.length - 1]).toBe(1)
    for (const pct of progressValues) {
      expect(pct).toBeGreaterThanOrEqual(0)
      expect(pct).toBeLessThanOrEqual(1)
    }
  })

  it('should parse frames correctly from chunked stream', async () => {
    const file = createStreamableFile(SAMPLE_ILR)
    await engine.loadFile(file)

    // Seek past the ts=3000 cpu_utilization frame so it gets emitted
    engine.seek(3500)
    const latest = engine.getLatest('cpu_utilization')
    expect(latest).not.toBeNull()
    expect(latest!.feature).toBe('cpu_utilization')
  })

  it('should handle single-line files in streaming mode', async () => {
    const content = '{"ts":1000,"feature":"test","data":{"val":1}}'
    const file = createStreamableFile(content)
    const meta = await engine.loadFile(file)
    expect(meta.frameCount).toBe(1)
  })

  it('should handle large number of frames efficiently', async () => {
    const lines: string[] = []
    for (let i = 0; i < 1000; i++) {
      lines.push(`{"ts":${i * 100},"feature":"perf","data":{"v":${i}}}`)
    }
    // Add trailing newline to ensure last line is flushed in streaming mode
    const file = createStreamableFile(lines.join('\n') + '\n')
    const start = performance.now()
    const meta = await engine.loadFile(file)
    const elapsed = performance.now() - start

    expect(meta.frameCount).toBe(1000)
    expect(elapsed).toBeLessThan(500)
  })
})
