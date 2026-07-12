export interface DataPoint {
  time: number
  [key: string]: number
}

const MAX_MEMORY_BYTES = 50 * 1024 * 1024
const BYTES_PER_NUMBER = 16
const OBJECT_OVERHEAD_BYTES = 80

function estimatePointSize(point: DataPoint): number {
  const keys = Object.keys(point)
  return OBJECT_OVERHEAD_BYTES + keys.length * BYTES_PER_NUMBER
}

/**
 * 固定容量环形缓冲。
 *
 * - push / pushMany 均摊 O(1)，不使用 Array.unshift/shift。
 * - 按秒级时间戳去重：同秒内新点会合并到最新点上（后到的字段覆盖先到）。
 * - 估算内存占用，超过 50MB 时从旧端自动清理。
 * - getAll / getRange / latest 均返回按时间从早到晚排序的数组/点。
 */
class RingBuffer {
  private buffer: DataPoint[]
  private head = 0
  private count = 0
  private readonly capacity: number
  private estimatedBytes = 0

  constructor(capacity: number) {
    this.capacity = capacity
    this.buffer = new Array(capacity)
  }

  push(point: DataPoint) {
    const normalized: DataPoint = { ...point, time: Math.floor(point.time) }

    // 同秒去重：与最新点合并
    if (this.count > 0) {
      const latest = this.buffer[this.index(this.count - 1)]
      if (latest && Math.floor(latest.time / 1000) === Math.floor(normalized.time / 1000)) {
        const oldSize = estimatePointSize(latest)
        Object.assign(latest, normalized)
        this.estimatedBytes += estimatePointSize(latest) - oldSize
        this.enforceMemoryLimit()
        return
      }
    }

    const pointSize = estimatePointSize(normalized)

    if (this.count === this.capacity) {
      const evicted = this.buffer[this.head]
      this.estimatedBytes -= estimatePointSize(evicted)
      this.head = (this.head + 1) % this.capacity
    } else {
      this.count++
    }

    const idx = this.index(this.count - 1)
    this.buffer[idx] = normalized
    this.estimatedBytes += pointSize
    this.enforceMemoryLimit()
  }

  pushMany(points: DataPoint[]) {
    for (const p of points) this.push(p)
  }

  query(start: number, end: number): DataPoint[] {
    return this.toArray().filter(p => p.time >= start && p.time <= end)
  }

  getRange(startTs: number, endTs: number): DataPoint[] {
    const data = this.toArray()
    let lo = 0
    let hi = data.length
    while (lo < hi) {
      const mid = (lo + hi) >>> 1
      if (data[mid].time < startTs) lo = mid + 1
      else hi = mid
    }
    const startIdx = lo
    hi = data.length
    while (lo < hi) {
      const mid = (lo + hi) >>> 1
      if (data[mid].time <= endTs) lo = mid + 1
      else hi = mid
    }
    return data.slice(startIdx, lo)
  }

  latest(): DataPoint | null {
    if (this.count === 0) return null
    return this.buffer[this.index(this.count - 1)]
  }

  all(): DataPoint[] {
    return this.toArray()
  }

  size(): number {
    return this.count
  }

  clear() {
    this.head = 0
    this.count = 0
    this.estimatedBytes = 0
    this.buffer = new Array(this.capacity)
  }

  memoryBytes(): number {
    return this.estimatedBytes
  }

  private index(i: number): number {
    return (this.head + i) % this.capacity
  }

  private toArray(): DataPoint[] {
    const out: DataPoint[] = new Array(this.count)
    for (let i = 0; i < this.count; i++) {
      out[i] = this.buffer[this.index(i)]
    }
    return out
  }

  private enforceMemoryLimit() {
    while (this.count > 0 && this.estimatedBytes > MAX_MEMORY_BYTES) {
      const evicted = this.buffer[this.head]
      this.estimatedBytes -= estimatePointSize(evicted)
      this.head = (this.head + 1) % this.capacity
      this.count--
    }
  }
}

const MAX_POINTS = 1800

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

  appendMany(key: string, points: DataPoint[]) {
    let buf = this.series.get(key)
    if (!buf) {
      buf = new RingBuffer(MAX_POINTS)
      this.series.set(key, buf)
    }
    buf.pushMany(points)
    this.notify(key)
  }

  query(key: string, start: number, end: number): DataPoint[] {
    const buf = this.series.get(key)
    if (!buf) return []
    return buf.query(start, end)
  }

  getRange(key: string, startTs: number, endTs: number): DataPoint[] {
    const buf = this.series.get(key)
    if (!buf) return []
    return buf.getRange(startTs, endTs)
  }

  latest(key: string): DataPoint | null {
    const buf = this.series.get(key)
    return buf?.latest() ?? null
  }

  all(key: string): DataPoint[] {
    const buf = this.series.get(key)
    return buf?.all() ?? []
  }

  size(key: string): number {
    return this.series.get(key)?.size() ?? 0
  }

  memoryBytes(key: string): number {
    return this.series.get(key)?.memoryBytes() ?? 0
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
}

export const timeSeriesStore = new TimeSeriesStoreImpl()
