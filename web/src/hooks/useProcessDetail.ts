import { useState, useRef, useEffect, useCallback } from 'react'
import { api } from '../services/apiClient'
import type { ThreadEntry } from '../components/charts/ThreadBreakdown'

interface ProcessTimelinePoint {
  timestamp: number
  cpu_user_pct: number
  cpu_sys_pct: number
}

interface CpuCollectResponse {
  pipeline: string
  records: Array<{
    labels: Record<string, string>
    fields: Record<string, number | string>
  }>
}

export function useProcessDetail(pid: number, active: boolean, intervalMs = 2000) {
  const [timeline, setTimeline] = useState<ProcessTimelinePoint[]>([])
  const [threads, setThreads] = useState<ThreadEntry[]>([])
  const timelineRef = useRef<ProcessTimelinePoint[]>([])

  useEffect(() => {
    if (!active || !pid) return

    let cancelled = false
    const poll = async () => {
      try {
        const resp = await api.featureCollect('cpu_processes') as CpuCollectResponse
        if (cancelled || !resp?.records) return

        const now = Date.now()
        let foundProcess = false
        const threadList: ThreadEntry[] = []

        for (const rec of resp.records) {
          const recPid = parseInt(rec.labels?.pid ?? '0', 10)
          if (recPid !== pid) continue

          if (rec.labels?.type === 'process') {
            foundProcess = true
            const point: ProcessTimelinePoint = {
              timestamp: now,
              cpu_user_pct: (rec.fields?.cpu_user_pct as number) ?? 0,
              cpu_sys_pct: (rec.fields?.cpu_sys_pct as number) ?? 0,
            }
            timelineRef.current.push(point)
            if (timelineRef.current.length > 60) timelineRef.current.shift()
            setTimeline([...timelineRef.current])
          } else if (rec.labels?.type === 'thread') {
            threadList.push({
              tid: parseInt(rec.labels?.tid ?? '0', 10),
              comm: (rec.labels?.comm as string) ?? '',
              cpu_total_pct: (rec.fields?.cpu_total_pct as number) ?? 0,
              cpu_user_pct: (rec.fields?.cpu_user_pct as number) ?? 0,
              cpu_sys_pct: (rec.fields?.cpu_sys_pct as number) ?? 0,
              state: (rec.fields?.state as string) ?? '?',
            })
          }
        }

        if (threadList.length > 0) setThreads(threadList)
        if (!foundProcess && timelineRef.current.length > 0) {
          const point: ProcessTimelinePoint = {
            timestamp: now, cpu_user_pct: 0, cpu_sys_pct: 0,
          }
          timelineRef.current.push(point)
          if (timelineRef.current.length > 60) timelineRef.current.shift()
          setTimeline([...timelineRef.current])
        }
      } catch {
        // retry
      }
    }

    poll()
    const timer = setInterval(poll, intervalMs)
    return () => { cancelled = true; clearInterval(timer) }
  }, [pid, active, intervalMs])

  const clear = useCallback(() => {
    timelineRef.current = []
    setTimeline([])
    setThreads([])
  }, [])

  return { timeline, threads, clear }
}
