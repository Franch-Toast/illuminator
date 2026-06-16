import { useEffect, useCallback, useMemo } from 'react'
import { useSearchParams } from 'react-router-dom'
import { useTimeStore, type TimeRange } from '../stores/useTimeStore'

interface UrlStateOptions {
  pid?: number | null
  comm?: string | null
  profileType?: string | null
  subTab?: string | null
}

interface UrlStateResult {
  pid: number | null
  comm: string | null
  profileType: string | null
  subTab: string | null
  setUrlState: (updates: Partial<UrlStateOptions>) => void
  clearUrlState: () => void
}

export function useUrlState(): UrlStateResult {
  const [searchParams, setSearchParams] = useSearchParams()
  const mode = useTimeStore(s => s.mode)
  const windowMs = useTimeStore(s => s.windowMs)
  const range: TimeRange = useTimeStore(s => s.range)

  const pid = useMemo(() => {
    const v = searchParams.get('pid')
    return v ? parseInt(v, 10) : null
  }, [searchParams])

  const comm = useMemo(() => searchParams.get('comm'), [searchParams])
  const profileType = useMemo(() => searchParams.get('profile'), [searchParams])
  const subTab = useMemo(() => searchParams.get('tab'), [searchParams])

  useEffect(() => {
    const currentMode = searchParams.get('mode')
    const currentWindow = searchParams.get('window')
    const shouldUpdateMode = mode === 'paused' && currentMode !== 'paused'
    const shouldUpdateWindow = String(windowMs) !== currentWindow && currentWindow !== null

    if (shouldUpdateMode || shouldUpdateWindow) {
      setSearchParams(prev => {
        const next = new URLSearchParams(prev)
        if (mode === 'paused') {
          next.set('mode', 'paused')
          if (range.start && range.end) {
            next.set('t', `${range.start}-${range.end}`)
          }
        } else {
          next.delete('mode')
          next.delete('t')
        }
        return next
      }, { replace: true })
    }
  }, [mode, windowMs, range.start, range.end, searchParams, setSearchParams])

  const setUrlState = useCallback((updates: Partial<UrlStateOptions>) => {
    setSearchParams(prev => {
      const next = new URLSearchParams(prev)
      if (updates.pid !== undefined) {
        if (updates.pid) next.set('pid', String(updates.pid))
        else next.delete('pid')
      }
      if (updates.comm !== undefined) {
        if (updates.comm) next.set('comm', updates.comm)
        else next.delete('comm')
      }
      if (updates.profileType !== undefined) {
        if (updates.profileType) next.set('profile', updates.profileType)
        else next.delete('profile')
      }
      if (updates.subTab !== undefined) {
        if (updates.subTab && updates.subTab !== 'system') next.set('tab', updates.subTab)
        else next.delete('tab')
      }
      return next
    }, { replace: true })
  }, [setSearchParams])

  const clearUrlState = useCallback(() => {
    setSearchParams({}, { replace: true })
  }, [setSearchParams])

  return { pid, comm, profileType, subTab, setUrlState, clearUrlState }
}
