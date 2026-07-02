import { useState, useRef, useEffect, useCallback } from 'react'
import { useTimeStore } from '../stores/useTimeStore'
import { getDataSource } from './useDataSource'
import type { DataBatch } from '../services/dataSource'
import type { ThreadEntry } from '../components/charts/ThreadBreakdown'
import { extractRecords } from '../utils/ssePayload'

interface ProcessTimelinePoint {
  timestamp: number
  cpu_user_pct: number
  cpu_sys_pct: number
}

export function useProcessDetail(pid: number, active: boolean) {
  const [timeline, setTimeline] = useState<ProcessTimelinePoint[]>([])
  const [threads, setThreads] = useState<ThreadEntry[]>([])
  const [processGone, setProcessGone] = useState(false)
  const timelineRef = useRef<ProcessTimelinePoint[]>([])
  const missCountRef = useRef(0)
  const mode = useTimeStore(s => s.mode)

  useEffect(() => {
    if (!active || !pid || mode === 'paused') return

    const source = getDataSource()
    const unsub = source.subscribe('process_cpu', (batch: DataBatch) => {
      const records = extractRecords(batch.data)
      if (records.length === 0) return

      const now = Date.now()
      let foundProcess = false
      const threadList: ThreadEntry[] = []

      for (const rec of records) {
        const recPid = parseInt(rec.labels?.pid ?? '0', 10)
        if (recPid !== pid) continue

        if (rec.labels?.type === 'process') {
          foundProcess = true
          missCountRef.current = 0
          setProcessGone(false)
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

      if (!foundProcess) {
        missCountRef.current++
        if (missCountRef.current >= 5) setProcessGone(true)
        const point: ProcessTimelinePoint = {
          timestamp: now, cpu_user_pct: 0, cpu_sys_pct: 0,
        }
        timelineRef.current.push(point)
        if (timelineRef.current.length > 60) timelineRef.current.shift()
        setTimeline([...timelineRef.current])
      }
    })

    return unsub
  }, [pid, active, mode])

  const clear = useCallback(() => {
    timelineRef.current = []
    setTimeline([])
    setThreads([])
    missCountRef.current = 0
    setProcessGone(false)
  }, [])

  return { timeline, threads, processGone, clear }
}
