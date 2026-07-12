/**
 * 数据降采样工具。
 *
 * 大数据量时降低图表渲染开销。
 * - lttb: Largest Triangle Three Buckets，保留视觉特征。
 * - interval: 等间隔采样，计算开销最小。
 */

export interface Point2D {
  x: number
  y: number
}

export interface DownsampleOptions {
  threshold?: number
  algorithm?: 'lttb' | 'interval'
}

/**
 * Largest Triangle Three Buckets 降采样算法。
 * 基于 Sveinn Steinarsson 的论文实现。
 */
export function lttb<T extends Point2D>(data: T[], threshold = 1000): T[] {
  const len = data.length
  if (len <= threshold || threshold <= 2) return data

  const sampled: T[] = [data[0]]
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
      avgY += data[j].y
    }
    avgY /= avgRangeLength

    let rangeOffs = Math.floor(i * every) + 1
    let rangeTo = Math.floor((i + 1) * every) + 1
    if (rangeTo >= len) rangeTo = len - 1
    if (rangeOffs >= rangeTo) rangeOffs = rangeTo - 1

    const pointAX = data[a].x
    const pointAY = data[a].y

    let maxArea = -1
    let maxAreaPoint = data[rangeOffs]

    for (let j = rangeOffs; j < rangeTo; j++) {
      const area = Math.abs(
        (pointAX - 0) * (data[j].y - pointAY) -
        (pointAX - data[j].x) * (avgY - pointAY)
      )
      if (area > maxArea) {
        maxArea = area
        maxAreaPoint = data[j]
      }
    }

    sampled.push(maxAreaPoint)
    a = rangeTo - 1
  }

  sampled.push(data[len - 1])
  return sampled
}

/**
 * 等间隔采样。保留首尾的简单策略。
 */
export function intervalSample<T>(data: T[], threshold = 1000): T[] {
  if (data.length <= threshold) return data
  const sampled: T[] = [data[0]]
  const step = (data.length - 1) / (threshold - 1)
  for (let i = 1; i < threshold - 1; i++) {
    sampled.push(data[Math.round(i * step)])
  }
  sampled.push(data[data.length - 1])
  return sampled
}

export function downsample<T extends Point2D>(data: T[], options: DownsampleOptions = {}): T[] {
  const { threshold = 1000, algorithm = 'lttb' } = options
  if (data.length <= threshold) return data
  return algorithm === 'lttb' ? lttb(data, threshold) : intervalSample(data, threshold)
}
