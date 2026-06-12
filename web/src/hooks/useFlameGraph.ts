import { useState, useRef, useCallback, useEffect } from 'react'
import type { FlameNode, StackSample } from '../workers/flameGraphWorker'

let worker: Worker | null = null
let pendingCallbacks = new Map<string, (result: unknown) => void>()

function getWorker(): Worker {
  if (!worker) {
    worker = new Worker(
      new URL('../workers/flameGraphWorker.ts', import.meta.url),
      { type: 'module' }
    )
    worker.onmessage = (e) => {
      const { id, payload } = e.data
      const cb = pendingCallbacks.get(id)
      if (cb) {
        pendingCallbacks.delete(id)
        cb(payload)
      }
    }
  }
  return worker
}

function sendToWorker<T>(type: string, payload: unknown): Promise<T> {
  return new Promise((resolve) => {
    const id = `${type}_${Date.now()}_${Math.random().toString(36).slice(2)}`
    pendingCallbacks.set(id, resolve as (r: unknown) => void)
    getWorker().postMessage({ type, id, payload })
  })
}

export function useFlameTree(samples: StackSample[]) {
  const [root, setRoot] = useState<FlameNode | null>(null)
  const [loading, setLoading] = useState(false)
  const versionRef = useRef(0)

  useEffect(() => {
    if (samples.length === 0) {
      setRoot(null)
      return
    }

    const version = ++versionRef.current
    setLoading(true)

    sendToWorker<FlameNode>('build', { samples }).then((result) => {
      if (versionRef.current === version) {
        setRoot(result)
        setLoading(false)
      }
    })
  }, [samples])

  return { root, loading }
}

export function useFlameSearch(root: FlameNode | null) {
  const [query, setQuery] = useState('')
  const [matches, setMatches] = useState<string[]>([])
  const [totalPct, setTotalPct] = useState(0)

  const search = useCallback((pattern: string) => {
    setQuery(pattern)
    if (!root || !pattern) {
      setMatches([])
      setTotalPct(0)
      return
    }

    sendToWorker<{ matches: string[]; totalPct: number }>('search', { root, pattern })
      .then((result) => {
        setMatches(result.matches)
        setTotalPct(result.totalPct)
      })
  }, [root])

  return { query, search, matches, totalPct }
}

export function useDiffFlameTree(
  baseline: StackSample[] | null,
  current: StackSample[] | null,
) {
  const [diffRoot, setDiffRoot] = useState<(FlameNode & { diff?: number }) | null>(null)
  const [loading, setLoading] = useState(false)

  useEffect(() => {
    if (!baseline || !current || baseline.length === 0 || current.length === 0) {
      setDiffRoot(null)
      return
    }

    setLoading(true)
    sendToWorker<FlameNode & { diff?: number }>('diff', { baseline, current })
      .then((result) => {
        setDiffRoot(result)
        setLoading(false)
      })
  }, [baseline, current])

  return { diffRoot, loading }
}

export interface FlameViewState {
  zoomNode: FlameNode | null
  breadcrumbs: FlameNode[]
  zoomIn: (node: FlameNode) => void
  zoomOut: () => void
  reset: () => void
}

export function useFlameZoom(root: FlameNode | null): FlameViewState {
  const [breadcrumbs, setBreadcrumbs] = useState<FlameNode[]>([])

  const zoomNode = breadcrumbs.length > 0 ? breadcrumbs[breadcrumbs.length - 1] : root

  const zoomIn = useCallback((node: FlameNode) => {
    setBreadcrumbs(prev => [...prev, node])
  }, [])

  const zoomOut = useCallback(() => {
    setBreadcrumbs(prev => prev.slice(0, -1))
  }, [])

  const reset = useCallback(() => {
    setBreadcrumbs([])
  }, [])

  useEffect(() => {
    setBreadcrumbs([])
  }, [root])

  return { zoomNode, breadcrumbs, zoomIn, zoomOut, reset }
}
