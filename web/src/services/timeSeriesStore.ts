export interface DataPoint {
  time: number
  [key: string]: number
}

class RingBuffer {
  private data: DataPoint[] = []
  private maxSize: number

  constructor(maxSize: number) {
    this.maxSize = maxSize
  }

  push(point: DataPoint) {
    this.data.push(point)
    if (this.data.length > this.maxSize) {
      this.data = this.data.slice(-this.maxSize)
    }
  }

  query(start: number, end: number): DataPoint[] {
    return this.data.filter(p => p.time >= start && p.time <= end)
  }

  latest(): DataPoint | null {
    return this.data.length > 0 ? this.data[this.data.length - 1]! : null
  }

  all(): DataPoint[] {
    return this.data
  }

  size(): number {
    return this.data.length
  }

  clear() {
    this.data = []
  }
}

const MAX_POINTS = 1800
const MAX_AGE_MS = 30 * 60 * 1000

class TimeSeriesStoreImpl {
  private series = new Map<string, RingBuffer>()
  private listeners = new Map<string, Set<() => void>>()

  append(key: string, time: number, values: Record<string, number>) {
    let buf = this.series.get(key)
    if (!buf) {
      buf = new RingBuffer(MAX_POINTS)
      this.series.set(key, buf)
    }
    buf.push({ time, ...values })
    this.notify(key)
  }

  query(key: string, start: number, end: number): DataPoint[] {
    const buf = this.series.get(key)
    if (!buf) return []
    return buf.query(start, end)
  }

  latest(key: string): DataPoint | null {
    const buf = this.series.get(key)
    return buf?.latest() ?? null
  }

  all(key: string): DataPoint[] {
    const buf = this.series.get(key)
    return buf?.all() ?? []
  }

  subscribe(key: string, callback: () => void): () => void {
    let set = this.listeners.get(key)
    if (!set) {
      set = new Set()
      this.listeners.set(key, set)
    }
    set.add(callback)
    return () => set!.delete(callback)
  }

  private notify(key: string) {
    const set = this.listeners.get(key)
    if (set) {
      for (const cb of set) cb()
    }
  }

  gc() {
    const cutoff = Date.now() - MAX_AGE_MS
    for (const [, buf] of this.series) {
      const data = buf.all()
      while (data.length > 0 && data[0]!.time < cutoff) {
        data.shift()
      }
    }
  }
}

export const timeSeriesStore = new TimeSeriesStoreImpl()
