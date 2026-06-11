import { useState, useRef, useCallback, useEffect } from 'react'
import { api, FeatureEntry } from '../services/apiClient'

export type FeatureState = 'inactive' | 'active' | 'paused' | 'starting' | 'stopping'

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

interface UseFeatureStreamOptions {
  windowSec?: number
  pollIntervalMs?: number
}

export function useFeatureStream<T extends { timestamp: number } = { timestamp: number; [key: string]: unknown }>(
  featureName: string,
  options: UseFeatureStreamOptions = {}
) {
  const { windowSec = 60, pollIntervalMs = 2000 } = options
  const [state, setState] = useState<FeatureState>('inactive')
  const [error, setError] = useState<string | null>(null)
  const [dataVersion, setDataVersion] = useState(0)
  const buffer = useRef(new TimeSeriesBuffer<T>(windowSec))
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)

  const start = useCallback(async () => {
    try {
      setError(null)
      setState('starting')
      await api.featureStart(featureName)
      setState('active')
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to start feature')
      setState('inactive')
    }
  }, [featureName])

  const stop = useCallback(async () => {
    try {
      setError(null)
      setState('stopping')
      await api.featureStop(featureName)
      setState('inactive')
      buffer.current.clear()
      setDataVersion(v => v + 1)
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to stop feature')
    }
  }, [featureName])

  const pause = useCallback(async () => {
    try {
      setError(null)
      await api.featurePause(featureName)
      setState('paused')
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to pause feature')
    }
  }, [featureName])

  const resume = useCallback(async () => {
    try {
      setError(null)
      await api.featureResume(featureName)
      setState('active')
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to resume feature')
    }
  }, [featureName])

  useEffect(() => {
    if (state !== 'active') {
      if (pollRef.current) {
        clearInterval(pollRef.current)
        pollRef.current = null
      }
      return
    }

    const poll = async () => {
      try {
        const data = await api.featureCollect(featureName) as T
        if (data && typeof data === 'object') {
          const item = { ...data, timestamp: Date.now() } as T
          buffer.current.push(item)
          setDataVersion(v => v + 1)
        }
      } catch {
        // Silently retry on next interval
      }
    }

    poll()
    pollRef.current = setInterval(poll, pollIntervalMs)
    return () => {
      if (pollRef.current) {
        clearInterval(pollRef.current)
        pollRef.current = null
      }
    }
  }, [state, featureName, pollIntervalMs])

  // Recording management
  const [isRecording, setIsRecording] = useState(false)

  const toggleRecording = useCallback(async () => {
    try {
      if (isRecording) {
        await api.featureRecordStop(featureName)
        setIsRecording(false)
      } else {
        await api.featureRecordStart(featureName)
        setIsRecording(true)
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Recording operation failed')
    }
  }, [featureName, isRecording])

  // Reset recording state when feature stops
  useEffect(() => {
    if (state === 'inactive') setIsRecording(false)
  }, [state])

  return {
    state,
    error,
    buffer: buffer.current,
    dataVersion,
    start,
    stop,
    pause,
    resume,
    isRecording,
    toggleRecording,
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
    refresh()
    const timer = setInterval(refresh, 3000)
    return () => clearInterval(timer)
  }, [refresh])

  return { features, loading, refresh }
}
