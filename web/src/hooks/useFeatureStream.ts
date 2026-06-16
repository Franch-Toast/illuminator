import { useState, useCallback, useEffect } from 'react'
import { api, FeatureEntry } from '../services/apiClient'

export class TimeSeriesBuffer<T extends { timestamp: number }> {
  private buffer: T[] = []
  private readonly windowMs: number

  constructor(windowSec: number) {
    this.windowMs = windowSec * 1000
  }

  push(item: T): void {
    this.buffer.push(item)
    this.evict()
  }

  pushMany(items: T[]): void {
    this.buffer.push(...items)
    this.evict()
  }

  private evict(): void {
    const cutoff = Date.now() - this.windowMs
    let lo = 0
    let hi = this.buffer.length
    while (lo < hi) {
      const mid = (lo + hi) >>> 1
      if (this.buffer[mid].timestamp < cutoff) lo = mid + 1
      else hi = mid
    }
    if (lo > 0) this.buffer.splice(0, lo)
  }

  getAll(): T[] {
    return this.buffer
  }

  getLast(n: number): T[] {
    return this.buffer.slice(-n)
  }

  get length(): number {
    return this.buffer.length
  }

  clear(): void {
    this.buffer = []
  }
}

export function useFeatureList() {
  const [features, setFeatures] = useState<FeatureEntry[]>([])
  const [loading, setLoading] = useState(true)

  const refresh = useCallback(async () => {
    try {
      const result = await api.features()
      setFeatures(result.features || [])
    } catch {
      // Keep existing state on error
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    refresh() // eslint-disable-line react-hooks/set-state-in-effect
    const timer = setInterval(refresh, 3000)
    return () => clearInterval(timer)
  }, [refresh])

  return { features, loading, refresh }
}
