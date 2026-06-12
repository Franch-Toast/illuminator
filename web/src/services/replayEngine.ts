import type { DataBatch, DataCallback, DataSource } from './dataSource'

interface IndexedFrame {
  timestamp: number
  feature: string
  offset: number
  length: number
}

export interface ReplayMeta {
  version: number
  features: string[]
  startTs: number
  endTs: number
  frameCount: number
  fileSizeBytes: number
}

export type PlaybackState = 'idle' | 'loading' | 'ready' | 'playing' | 'paused'

export class ReplayEngine implements DataSource {
  private lines: string[] = []
  private frames: IndexedFrame[] = []
  private meta: ReplayMeta | null = null
  private currentIndex = 0
  private speed = 1
  private state: PlaybackState = 'idle'
  private animFrame: number | null = null
  private lastRealTime = 0
  private currentSimTime = 0
  private subscribers = new Map<string, Set<DataCallback>>()
  private latestByFeature = new Map<string, DataBatch>()
  private stateListeners = new Set<(s: PlaybackState) => void>()

  async loadFile(file: File): Promise<ReplayMeta> {
    this.state = 'loading'
    this.notifyState()

    const text = await file.text()
    this.lines = text.split('\n').filter(l => l.trim())

    this.frames = []
    let startTs = Infinity, endTs = 0
    const features = new Set<string>()

    for (let i = 0; i < this.lines.length; i++) {
      try {
        const obj = JSON.parse(this.lines[i])
        if (obj.type === 'header') {
          if (obj.features) obj.features.forEach((f: string) => features.add(f))
          continue
        }
        if (!obj.ts || !obj.feature) continue

        features.add(obj.feature)
        if (obj.ts < startTs) startTs = obj.ts
        if (obj.ts > endTs) endTs = obj.ts

        this.frames.push({
          timestamp: obj.ts,
          feature: obj.feature,
          offset: i,
          length: 1,
        })
      } catch { continue }
    }

    this.frames.sort((a, b) => a.timestamp - b.timestamp)

    this.meta = {
      version: 1,
      features: Array.from(features),
      startTs,
      endTs,
      frameCount: this.frames.length,
      fileSizeBytes: file.size,
    }

    this.state = 'ready'
    this.currentIndex = 0
    this.currentSimTime = startTs
    this.notifyState()

    return this.meta
  }

  play() {
    if (this.state !== 'ready' && this.state !== 'paused') return
    this.state = 'playing'
    this.lastRealTime = performance.now()
    this.notifyState()
    this.tick()
  }

  pause() {
    if (this.state !== 'playing') return
    this.state = 'paused'
    if (this.animFrame) cancelAnimationFrame(this.animFrame)
    this.animFrame = null
    this.notifyState()
  }

  seek(timestamp: number) {
    if (!this.meta) return
    const clamped = Math.max(this.meta.startTs, Math.min(this.meta.endTs, timestamp))
    this.currentSimTime = clamped

    let lo = 0, hi = this.frames.length
    while (lo < hi) {
      const mid = (lo + hi) >>> 1
      if (this.frames[mid].timestamp < clamped) lo = mid + 1
      else hi = mid
    }
    this.currentIndex = lo

    this.emitCurrentFrame()
  }

  setSpeed(speed: number) {
    this.speed = speed
  }

  getState(): PlaybackState {
    return this.state
  }

  getMeta(): ReplayMeta | null {
    return this.meta
  }

  getCurrentTime(): number {
    return this.currentSimTime
  }

  getProgress(): number {
    if (!this.meta) return 0
    const range = this.meta.endTs - this.meta.startTs
    if (range <= 0) return 0
    return (this.currentSimTime - this.meta.startTs) / range
  }

  onStateChange(cb: (s: PlaybackState) => void): () => void {
    this.stateListeners.add(cb)
    return () => { this.stateListeners.delete(cb) }
  }

  // DataSource interface
  subscribe(feature: string, cb: DataCallback): () => void {
    let set = this.subscribers.get(feature)
    if (!set) {
      set = new Set()
      this.subscribers.set(feature, set)
    }
    set.add(cb)
    return () => {
      set!.delete(cb)
      if (set!.size === 0) this.subscribers.delete(feature)
    }
  }

  getLatest(feature: string): DataBatch | null {
    return this.latestByFeature.get(feature) ?? null
  }

  getAvailableFeatures(): string[] {
    return this.meta?.features ?? []
  }

  destroy() {
    if (this.animFrame) cancelAnimationFrame(this.animFrame)
    this.subscribers.clear()
    this.stateListeners.clear()
    this.lines = []
    this.frames = []
  }

  private tick() {
    if (this.state !== 'playing') return

    const now = performance.now()
    const realDelta = now - this.lastRealTime
    this.lastRealTime = now

    this.currentSimTime += realDelta * this.speed

    while (this.currentIndex < this.frames.length &&
           this.frames[this.currentIndex].timestamp <= this.currentSimTime) {
      this.emitFrame(this.currentIndex)
      this.currentIndex++
    }

    if (this.currentIndex >= this.frames.length) {
      this.state = 'ready'
      this.notifyState()
      return
    }

    this.animFrame = requestAnimationFrame(() => this.tick())
  }

  private emitCurrentFrame() {
    if (this.currentIndex > 0 && this.currentIndex <= this.frames.length) {
      this.emitFrame(this.currentIndex - 1)
    }
  }

  private emitFrame(index: number) {
    const frame = this.frames[index]
    try {
      const obj = JSON.parse(this.lines[frame.offset])
      const batch: DataBatch = {
        feature: frame.feature,
        timestamp: frame.timestamp,
        data: obj.data ?? obj,
      }

      this.latestByFeature.set(frame.feature, batch)
      const subs = this.subscribers.get(frame.feature)
      if (subs) {
        for (const cb of subs) cb(batch)
      }
    } catch { /* skip malformed frames */ }
  }

  private notifyState() {
    for (const cb of this.stateListeners) cb(this.state)
  }
}
