/**
 * useProfileData — React hook for subscribing to profile-type Feature data (flamegraphs, stack samples)
 *
 * RFC v3 4.4: 根据 modelType 自动过滤 profile 类型数据
 */

import { useEffect, useState } from 'react'
import { dataBus } from '../services/dataBus'
import type { DataBatch, DataCallback } from '../services/dataSource'

export interface ProfileBatch {
  feature: string
  timestamp: number
  modelType: 'profile'
  stack_samples?: Array<{
    stack: string[]
    count: number
    pid?: number
    tid?: number
    comm?: string
  }>
  data: unknown
}

export function useProfileData(feature: string, maxSnapshots = 10): ProfileBatch[] {
  const [data, setData] = useState<ProfileBatch[]>([])

  useEffect(() => {
    const callback: DataCallback = (batch: DataBatch) => {
      if (batch.modelType === 'profile') {
        setData(prev => {
          const next = [...prev, batch as unknown as ProfileBatch]
          return next.length > maxSnapshots ? next.slice(-maxSnapshots) : next
        })
      }
    }

    const unsubscribe = dataBus.subscribe(feature, callback)
    return unsubscribe
  }, [feature, maxSnapshots])

  return data
}

export function useLatestProfile(feature: string): ProfileBatch | null {
  const [latest, setLatest] = useState<ProfileBatch | null>(null)

  useEffect(() => {
    const callback: DataCallback = (batch: DataBatch) => {
      if (batch.modelType === 'profile') {
        setLatest(batch as unknown as ProfileBatch)
      }
    }

    const unsubscribe = dataBus.subscribe(feature, callback)
    return unsubscribe
  }, [feature])

  return latest
}
