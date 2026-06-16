import { useState, useEffect, useRef, useCallback } from 'react'
import { useTimeStore } from '../../stores/useTimeStore'
import { colors } from '../../styles/theme'
import { api } from '../../services/apiClient'
import type { FlameNode, StackSample as WorkerSample } from '../../workers/flameGraphWorker'

interface StackSample {
  comm: string
  tid: number
  stack: string[]
  count: number
  timestamp: number
}

export interface TimeSelection {
  type: 'point' | 'range'
  start: number
  end: number
}

interface ProfileSnapshotProps {
  pid: number
  comm: string
  profileType: 'on_cpu' | 'off_cpu'
  selectedTimestamp: number | null
  timeSelection?: TimeSelection | null
  threadComms?: string[]
}

let worker: Worker | null = null
let pendingCallbacks = new Map<string, (result: unknown) => void>()

function getWorker(): Worker {
  if (!worker) {
    worker = new Worker(
      new URL('../../workers/flameGraphWorker.ts', import.meta.url),
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

function buildFlameTreeAsync(samples: WorkerSample[]): Promise<FlameNode> {
  return new Promise((resolve) => {
    const id = `build_${Date.now()}_${Math.random().toString(36).slice(2)}`
    pendingCallbacks.set(id, resolve as (r: unknown) => void)
    getWorker().postMessage({ type: 'build', id, payload: { samples } })
  })
}

export default function ProfileSnapshot({ pid, comm, profileType, timeSelection }: ProfileSnapshotProps) {
  const [root, setRoot] = useState<FlameNode | null>(null)
  const [topFunctions, setTopFunctions] = useState<{ name: string; pct: number }[]>([])
  const [totalSamples, setTotalSamples] = useState(0)
  const [filteredSamples, setFilteredSamples] = useState(0)
  const [pollCount, setPollCount] = useState(0)
  const [error, setError] = useState<string | null>(null)
  const accumulatedRef = useRef<StackSample[]>([])
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const cursorRef = useRef<number>(0)
  const abortRef = useRef<AbortController | null>(null)
  const buildVersionRef = useRef(0)
  const mode = useTimeStore(s => s.mode)

  const MAX_SAMPLES = 5000

  const processCollectedData = useCallback((rawSamples: Array<Record<string, unknown>>) => {
    if (rawSamples.length === 0) return

    const now = Date.now()

    const newSamples: StackSample[] = rawSamples
      .map((s) => {
        const kernelStack = (s.kernel_stack as Array<{ function_name?: string; address?: number }>) ?? []
        const userStack = (s.user_stack as Array<{ function_name?: string; address?: number }>) ?? []
        return {
          comm: (s.comm as string) ?? '',
          tid: (s.tid as number) ?? 0,
          count: (s.count as number) ?? ((s.duration_ns as number) ? Math.round((s.duration_ns as number) / 1000) : 1),
          timestamp: now,
          stack: [
            ...kernelStack.map(f => cleanFrameName(f.function_name, f.address)),
            ...userStack.map(f => cleanFrameName(f.function_name, f.address)),
          ].filter(f => f !== ''),
        }
      })

    if (newSamples.length > 0) {
      accumulatedRef.current = [...accumulatedRef.current, ...newSamples]
      if (accumulatedRef.current.length > MAX_SAMPLES) {
        accumulatedRef.current = accumulatedRef.current.slice(-MAX_SAMPLES)
      }
    }

    setTotalSamples(accumulatedRef.current.length)
    setPollCount(c => c + 1)
  }, [])

  useEffect(() => {
    const samples = accumulatedRef.current
    if (samples.length === 0) {
      setRoot(null)
      setFilteredSamples(0)
      return
    }

    let filtered: StackSample[]
    if (timeSelection) {
      filtered = samples.filter(s => s.timestamp >= timeSelection.start && s.timestamp <= timeSelection.end)
    } else {
      filtered = samples
    }

    setFilteredSamples(filtered.length)
    if (filtered.length > 0) {
      const version = ++buildVersionRef.current
      const workerSamples: WorkerSample[] = filtered.map(s => ({
        stack: s.stack,
        count: s.count,
      }))

      buildFlameTreeAsync(workerSamples).then((tree) => {
        if (buildVersionRef.current === version) {
          setRoot(tree)
          setTopFunctions(extractTopFunctions(tree, 10))
        }
      })
    } else {
      setRoot(null)
      setTopFunctions([])
    }
  }, [totalSamples, timeSelection])

  useEffect(() => {
    accumulatedRef.current = []
    setRoot(null)
    setTopFunctions([])
    setTotalSamples(0)
    setFilteredSamples(0)
    setPollCount(0)
    setError(null)

    const featureName = profileType === 'off_cpu' ? 'offcpu_profile' : 'cpu_profile'
    api.featureStream(featureName, 999999999)
      .then(data => {
        cursorRef.current = data?.cursor ?? 0
      })
      .catch(() => { cursorRef.current = 0 })
  }, [pid, profileType])

  useEffect(() => {
    if (mode === 'paused') {
      if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null }
      return
    }

    const featureName = profileType === 'off_cpu' ? 'offcpu_profile' : 'cpu_profile'

    const poll = async () => {
      abortRef.current?.abort()
      abortRef.current = new AbortController()

      try {
        const data = await api.featureStream(featureName, cursorRef.current, abortRef.current.signal)
        if (data.cursor) cursorRef.current = data.cursor
        const batches = data.batches ?? []
        for (const batch of batches) {
          const rawSamples = (batch as Record<string, unknown>).stack_samples as Array<Record<string, unknown>> ?? []
          processCollectedData(rawSamples)
        }
        if (batches.length === 0) {
          const fdata = await api.featureCollect(featureName) as Record<string, unknown>
          const rawSamples = (fdata.stack_samples as Array<Record<string, unknown>>) ?? []
          processCollectedData(rawSamples)
        }
      } catch (e) {
        if (e instanceof Error && e.name === 'AbortError') return
        setError(e instanceof Error ? e.message : 'Fetch failed')
      }
    }

    poll()
    timerRef.current = setInterval(poll, 1500)
    return () => {
      if (timerRef.current) clearInterval(timerRef.current)
      abortRef.current?.abort()
    }
  }, [pid, profileType, processCollectedData, mode])

  if (error) {
    return <div style={{ padding: 24, textAlign: 'center', color: colors.danger }}>{error}</div>
  }

  if (!root || root.value === 0) {
    return (
      <div style={{ padding: 32, textAlign: 'center', color: colors.textMuted }}>
        <div style={{ fontSize: 13, marginBottom: 4 }}>
          Collecting {profileType === 'on_cpu' ? 'On-CPU' : 'Off-CPU'} samples for {comm}...
        </div>
        <div style={{ fontSize: 11 }}>
          Polls: {pollCount} | Accumulated samples: {totalSamples}
        </div>
        <div style={{ fontSize: 11, marginTop: 4 }}>
          Waiting for target process to be sampled (profiling at 49Hz)
        </div>
      </div>
    )
  }

  return (
    <div style={{ width: '100%', overflow: 'hidden' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8, flexWrap: 'wrap', gap: 4 }}>
        <span style={{ fontSize: 12, color: colors.textMuted }}>
          {profileType === 'on_cpu' ? 'On-CPU' : 'Off-CPU'} Profile —{' '}
          {timeSelection
            ? `${filteredSamples}/${totalSamples} samples (filtered)`
            : `${totalSamples} samples`
          }
        </span>
        <div style={{ display: 'flex', gap: 4 }}>
          {timeSelection && (
            <span style={{ fontSize: 10, padding: '2px 6px', borderRadius: 3, background: 'rgba(96,165,250,0.15)', color: colors.accent }}>
              {timeSelection.type === 'point' ? '1s window' : `${((timeSelection.end - timeSelection.start) / 1000).toFixed(0)}s range`}
            </span>
          )}
          <button
            onClick={() => { accumulatedRef.current = []; cursorRef.current = 0; setRoot(null); setTotalSamples(0); setFilteredSamples(0) }}
            style={{ fontSize: 11, padding: '2px 8px', borderRadius: 3, border: `1px solid ${colors.cardBorder}`, background: 'transparent', color: colors.textMuted, cursor: 'pointer' }}
          >
            Reset
          </button>
        </div>
      </div>
      <div style={{ display: 'flex', gap: 16, width: '100%', minWidth: 0 }}>
        <div style={{ flex: 1, minWidth: 0, overflow: 'hidden' }}>
          <FlameGraph root={root} />
        </div>
        <div style={{ width: 220, flexShrink: 0 }}>
          <h5 style={{ margin: '0 0 8px', fontSize: 12, color: colors.textSecondary }}>Top Functions</h5>
          {topFunctions.map((fn, i) => (
            <div key={i} style={{ display: 'flex', justifyContent: 'space-between', padding: '3px 0', fontSize: 11, borderBottom: `1px solid ${colors.cardBorder}22` }}>
              <span style={{ color: colors.textPrimary, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', maxWidth: 160 }} title={fn.name}>
                {fn.name}
              </span>
              <span style={{ color: colors.accent, fontFamily: 'monospace', flexShrink: 0, marginLeft: 4 }}>
                {fn.pct.toFixed(1)}%
              </span>
            </div>
          ))}
        </div>
      </div>
    </div>
  )
}

function FlameGraph({ root }: { root: FlameNode }) {
  const containerRef = useRef<HTMLDivElement>(null)
  const maxDepth = 20
  const rows = flattenToRows(root, maxDepth)
  const totalValue = root.value
  const rowHeight = 18

  return (
    <div
      ref={containerRef}
      style={{
        width: '100%',
        maxWidth: '100%',
        overflow: 'hidden',
        display: 'flex',
        flexDirection: 'column',
        gap: 1,
        maxHeight: maxDepth * (rowHeight + 1),
        overflowY: 'auto',
      }}
    >
      {rows.map((row, depth) => (
        <div key={depth} style={{ display: 'flex', height: rowHeight, width: '100%', maxWidth: '100%', overflow: 'hidden', flexShrink: 0 }}>
          {row.map((node, i) => {
            const widthPct = (node.value / totalValue) * 100
            if (widthPct < 0.3) return null
            return (
              <div
                key={i}
                title={`${node.name}\n${node.value} samples (${widthPct.toFixed(1)}%)`}
                style={{
                  width: `${widthPct}%`,
                  minWidth: 0,
                  background: frameColor(node.name, depth),
                  borderRadius: 2,
                  padding: '0 3px',
                  overflow: 'hidden',
                  whiteSpace: 'nowrap',
                  textOverflow: 'ellipsis',
                  fontSize: 10,
                  lineHeight: `${rowHeight}px`,
                  color: '#fff',
                  cursor: 'pointer',
                  marginRight: 1,
                  boxSizing: 'border-box',
                }}
              >
                {widthPct > 4 ? node.name : ''}
              </div>
            )
          })}
        </div>
      ))}
    </div>
  )
}

function cleanFrameName(functionName?: string, address?: number): string {
  if (!functionName && !address) return ''
  if (!functionName || functionName === '0x0') {
    return address ? `0x${address.toString(16)}` : ''
  }

  let name = functionName

  const binaryOffsetMatch = name.match(/^\[(.+?)\+0x[0-9a-f]+\]$/)
  if (binaryOffsetMatch) {
    const path = binaryOffsetMatch[1]
    const binary = path.split('/').pop() ?? path
    return `[${binary}]`
  }

  if (name.match(/^\[0x[0-9a-f]+\]$/)) {
    return name.length > 14 ? name.slice(0, 14) + '…' : name
  }

  if (name.startsWith('[kernel ')) {
    return '[kernel]'
  }

  name = name.replace(/\s*\[inlined\]$/, '')

  const templateIdx = name.indexOf('<')
  if (templateIdx > 0 && name.length > 40) {
    name = name.slice(0, templateIdx) + '<…>'
  }

  return name
}

function flattenToRows(root: FlameNode, maxDepth: number): FlameNode[][] {
  const rows: FlameNode[][] = []
  const queue: { node: FlameNode; depth: number }[] = [{ node: root, depth: 0 }]

  while (queue.length > 0) {
    const { node, depth } = queue.shift()!
    if (depth >= maxDepth) continue
    if (!rows[depth]) rows[depth] = []
    rows[depth].push(node)
    for (const child of (node.children ?? [])) {
      queue.push({ node: child, depth: depth + 1 })
    }
  }
  return rows
}

function extractTopFunctions(root: FlameNode, n: number): { name: string; pct: number }[] {
  const map = new Map<string, number>()
  const total = root.value || 1

  function walk(node: FlameNode) {
    if (node.name !== 'root') {
      const self = node.value - (node.children?.reduce((s, c) => s + c.value, 0) ?? 0)
      if (self > 0) map.set(node.name, (map.get(node.name) ?? 0) + self)
    }
    for (const child of node.children ?? []) walk(child)
  }
  walk(root)

  return [...map.entries()]
    .sort((a, b) => b[1] - a[1])
    .slice(0, n)
    .map(([name, val]) => ({ name, pct: (val / total) * 100 }))
}

function frameColor(name: string, depth: number): string {
  if (name === 'root') return '#374151'
  if (name.startsWith('[kernel') || name.startsWith('__sched')) return '#7c3aed'
  if (name.includes('std::') || name.includes('__cxa')) return '#0d9488'
  if (name.includes('epoll') || name.includes('futex') || name.includes('poll')) return '#b45309'
  if (name.includes('schedule') || name.includes('wait')) return '#6d28d9'
  if (name.startsWith('[') && name.endsWith(']')) return '#4b5563'
  const hue = (hashCode(name) % 40) + 10
  const sat = 60 + (depth * 2)
  const lum = 38 + (depth * 1.5)
  return `hsl(${hue}, ${sat}%, ${lum}%)`
}

function hashCode(s: string): number {
  let h = 0
  for (let i = 0; i < s.length; i++) h = ((h << 5) - h + s.charCodeAt(i)) | 0
  return Math.abs(h)
}
