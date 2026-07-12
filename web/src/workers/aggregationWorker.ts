export interface TimeSeriesPoint {
  timestamp: number
  value: number
}

export interface DownsamplePayload {
  points: TimeSeriesPoint[]
  threshold: number
}

export interface StatsPayload {
  values: number[]
}

export interface AggregatePayload {
  buckets: number
  points: TimeSeriesPoint[]
}

export interface WorkerMessage {
  type: 'downsample' | 'stats' | 'aggregate'
  id: string
  payload: unknown
}

export interface WorkerResponse {
  type: 'result'
  id: string
  payload: unknown
}

/**
 * Largest Triangle Three Buckets 降采样。
 */
function lttb(points: TimeSeriesPoint[], threshold: number): TimeSeriesPoint[] {
  const len = points.length
  if (len <= threshold || threshold <= 2) return points

  const sampled: TimeSeriesPoint[] = [points[0]]
  const every = (len - 2) / (threshold - 2)

  let a = 0

  for (let i = 0; i < threshold - 2; i++) {
    let avgRangeStart = Math.floor((i + 1) * every) + 1
    let avgRangeEnd = Math.floor((i + 2) * every) + 1
    if (avgRangeEnd >= len) avgRangeEnd = len - 1
    if (avgRangeStart >= avgRangeEnd) avgRangeStart = avgRangeEnd - 1
    const avgRangeLength = avgRangeEnd - avgRangeStart

    let avgY = 0
    for (let j = avgRangeStart; j < avgRangeEnd; j++) {
      avgY += points[j].value
    }
    avgY /= avgRangeLength

    let rangeOffs = Math.floor(i * every) + 1
    let rangeTo = Math.floor((i + 1) * every) + 1
    if (rangeTo >= len) rangeTo = len - 1
    if (rangeOffs >= rangeTo) rangeOffs = rangeTo - 1

    const pointAX = points[a].timestamp
    const pointAY = points[a].value

    let maxArea = -1
    let maxAreaPoint = points[rangeOffs]

    for (let j = rangeOffs; j < rangeTo; j++) {
      const area = Math.abs(
        (pointAX - 0) * (points[j].value - pointAY) -
        (pointAX - points[j].timestamp) * (avgY - pointAY)
      )
      if (area > maxArea) {
        maxArea = area
        maxAreaPoint = points[j]
      }
    }

    sampled.push(maxAreaPoint)
    a = rangeTo - 1
  }

  sampled.push(points[len - 1])
  return sampled
}

function computeStats(values: number[]): { min: number; max: number; avg: number; p50: number; p99: number; sum: number; count: number } {
  const sorted = [...values].sort((a, b) => a - b)
  const count = sorted.length
  const sum = sorted.reduce((acc, v) => acc + v, 0)
  const min = sorted[0] ?? 0
  const max = sorted[count - 1] ?? 0
  const avg = count > 0 ? sum / count : 0
  const p50 = percentile(sorted, 0.5)
  const p99 = percentile(sorted, 0.99)
  return { min, max, avg, p50, p99, sum, count }
}

function percentile(sorted: number[], p: number): number {
  if (sorted.length === 0) return 0
  const idx = Math.max(0, Math.ceil(sorted.length * p) - 1)
  return sorted[idx]
}

function aggregateBuckets(buckets: number, points: TimeSeriesPoint[]): { timestamp: number; min: number; max: number; avg: number; count: number }[] {
  if (points.length === 0 || buckets <= 0) return []
  if (points.length <= buckets) {
    return points.map(p => ({ timestamp: p.timestamp, min: p.value, max: p.value, avg: p.value, count: 1 }))
  }

  const minTs = points[0].timestamp
  const maxTs = points[points.length - 1].timestamp
  const span = Math.max(1, maxTs - minTs)
  const bucketSpan = span / buckets
  const result: { timestamp: number; min: number; max: number; avg: number; count: number }[] = []

  let current: { sum: number; count: number; min: number; max: number; start: number } | null = null

  for (const p of points) {
    const bucketIdx = Math.min(buckets - 1, Math.floor((p.timestamp - minTs) / bucketSpan))
    const start = minTs + bucketIdx * bucketSpan

    if (!current || current.start !== start) {
      if (current) {
        result.push({
          timestamp: current.start,
          min: current.min,
          max: current.max,
          avg: current.count > 0 ? current.sum / current.count : 0,
          count: current.count,
        })
      }
      current = { sum: p.value, count: 1, min: p.value, max: p.value, start }
    } else {
      current.sum += p.value
      current.count++
      current.min = Math.min(current.min, p.value)
      current.max = Math.max(current.max, p.value)
    }
  }

  if (current) {
    result.push({
      timestamp: current.start,
      min: current.min,
      max: current.max,
      avg: current.count > 0 ? current.sum / current.count : 0,
      count: current.count,
    })
  }

  return result
}

self.onmessage = (e: MessageEvent<WorkerMessage>) => {
  const { type, id, payload } = e.data
  let result: unknown

  switch (type) {
    case 'downsample':
      result = lttb((payload as DownsamplePayload).points, (payload as DownsamplePayload).threshold)
      break
    case 'stats':
      result = computeStats((payload as StatsPayload).values)
      break
    case 'aggregate':
      result = aggregateBuckets((payload as AggregatePayload).buckets, (payload as AggregatePayload).points)
      break
  }

  const response: WorkerResponse = { type: 'result', id, payload: result }
  self.postMessage(response)
}
