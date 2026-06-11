import { useState, useEffect, useRef, useCallback } from 'react'
import { colors } from '../../styles/theme'

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

interface StackSample {
  comm: string
  tid: number
  stack: string[]
  count: number
}

interface ProfileSnapshotProps {
  pid: number
  comm: string
  profileType: 'on_cpu' | 'off_cpu'
  selectedTimestamp: number | null
  threadComms?: string[]
}

export default function ProfileSnapshot({ pid, comm, profileType, threadComms }: ProfileSnapshotProps) {
  const [root, setRoot] = useState<FlameNode | null>(null)
  const [topFunctions, setTopFunctions] = useState<{ name: string; pct: number }[]>([])
  const [totalSamples, setTotalSamples] = useState(0)
  const [pollCount, setPollCount] = useState(0)
  const [error, setError] = useState<string | null>(null)
  const accumulatedRef = useRef<StackSample[]>([])
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)

  const hostPidRef = useRef<number | null>(null)

  const processCollectedData = useCallback((rawSamples: Array<Record<string, unknown>>) => {
    // In containerized environments, BPF reports host-namespace PIDs which differ
    // from container-visible PIDs. Discover the host PID by finding samples whose
    // comm matches the target process name or any known thread name of the process.
    const knownComms = new Set<string>([comm, ...(threadComms ?? [])])

    if (hostPidRef.current === null) {
      for (const s of rawSamples) {
        const sComm = (s.comm as string) ?? ''
        if (knownComms.has(sComm)) {
          hostPidRef.current = s.pid as number
          break
        }
      }
    }

    const newSamples: StackSample[] = rawSamples
      .filter((s) => {
        const sPid = s.pid as number
        if (sPid === pid) return true
        if (hostPidRef.current !== null && sPid === hostPidRef.current) return true
        return false
      })
      .map((s) => {
        const kernelStack = (s.kernel_stack as Array<{ function_name?: string; address?: number }>) ?? []
        const userStack = (s.user_stack as Array<{ function_name?: string; address?: number }>) ?? []
        return {
          comm: (s.comm as string) ?? '',
          tid: (s.tid as number) ?? 0,
          count: (s.count as number) ?? ((s.duration_ns as number) ? Math.round((s.duration_ns as number) / 1000) : 1),
          stack: [
            ...kernelStack.map(f => f.function_name || `0x${(f.address ?? 0).toString(16)}`),
            ...userStack.map(f => f.function_name || `0x${(f.address ?? 0).toString(16)}`),
          ],
        }
      })

    if (newSamples.length > 0) {
      accumulatedRef.current = [...accumulatedRef.current, ...newSamples]
      const maxAccumulated = 2000
      if (accumulatedRef.current.length > maxAccumulated) {
        accumulatedRef.current = accumulatedRef.current.slice(-maxAccumulated)
      }
    }

    if (accumulatedRef.current.length > 0) {
      const tree = buildFlameTree(accumulatedRef.current)
      setRoot(tree)
      setTopFunctions(extractTopFunctions(tree, 10))
      setTotalSamples(accumulatedRef.current.length)
    }
  }, [pid, comm, threadComms])

  useEffect(() => {
    accumulatedRef.current = []
    hostPidRef.current = null
    setRoot(null)
    setTopFunctions([])
    setTotalSamples(0)
    setPollCount(0)
    setError(null)

    const featureName = profileType === 'off_cpu' ? 'offcpu_profile' : 'cpu_profile'

    const poll = async () => {
      try {
        const resp = await fetch(`/api/v1/features/${featureName}/collect`)
        if (!resp.ok) return
        const data = await resp.json()
        const rawSamples = data.stack_samples ?? []
        processCollectedData(rawSamples)
        setPollCount(c => c + 1)
      } catch (e) {
        setError(e instanceof Error ? e.message : 'Fetch failed')
      }
    }

    poll()
    timerRef.current = setInterval(poll, 2000)
    return () => {
      if (timerRef.current) clearInterval(timerRef.current)
    }
  }, [pid, profileType, processCollectedData])

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
          Waiting for target process to be sampled (system-wide profiling at 49Hz)
        </div>
      </div>
    )
  }

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 }}>
        <span style={{ fontSize: 12, color: colors.textMuted }}>
          {profileType === 'on_cpu' ? 'On-CPU' : 'Off-CPU'} Profile — {totalSamples} samples accumulated
        </span>
        <button
          onClick={() => { accumulatedRef.current = []; setRoot(null); setTotalSamples(0) }}
          style={{ fontSize: 11, padding: '2px 8px', borderRadius: 3, border: `1px solid ${colors.cardBorder}`, background: 'transparent', color: colors.textMuted, cursor: 'pointer' }}
        >
          Reset
        </button>
      </div>
      <div style={{ display: 'flex', gap: 16 }}>
        <div style={{ flex: 1 }}>
          <SimplifiedFlameGraph root={root} />
        </div>
        <div style={{ width: 240, flexShrink: 0 }}>
          <h5 style={{ margin: '0 0 8px', fontSize: 12, color: colors.textSecondary }}>Top Functions</h5>
          {topFunctions.map((fn, i) => (
            <div key={i} style={{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: 12, borderBottom: `1px solid ${colors.cardBorder}22` }}>
              <span style={{ color: colors.textPrimary, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', maxWidth: 180 }} title={fn.name}>
                {fn.name}
              </span>
              <span style={{ color: colors.accent, fontFamily: 'monospace', flexShrink: 0 }}>
                {fn.pct.toFixed(1)}%
              </span>
            </div>
          ))}
        </div>
      </div>
    </div>
  )
}

function SimplifiedFlameGraph({ root }: { root: FlameNode }) {
  const maxDepth = 12
  const rows = flattenToRows(root, maxDepth)
  const totalValue = root.value

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 1 }}>
      {rows.map((row, depth) => (
        <div key={depth} style={{ display: 'flex', height: 20 }}>
          {row.map((node, i) => {
            const widthPct = (node.value / totalValue) * 100
            if (widthPct < 0.5) return null
            return (
              <div
                key={i}
                title={`${node.name} (${node.value} samples, ${widthPct.toFixed(1)}%)`}
                style={{
                  width: `${widthPct}%`,
                  background: frameColor(node.name, depth),
                  borderRadius: 2,
                  padding: '0 4px',
                  overflow: 'hidden',
                  whiteSpace: 'nowrap',
                  textOverflow: 'ellipsis',
                  fontSize: 10,
                  lineHeight: '20px',
                  color: '#fff',
                  cursor: 'pointer',
                  marginRight: 1,
                }}
              >
                {widthPct > 3 ? node.name : ''}
              </div>
            )
          })}
        </div>
      ))}
    </div>
  )
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

function buildFlameTree(samples: StackSample[]): FlameNode {
  const root: FlameNode = { name: 'root', value: 0, children: [] }
  let total = 0

  for (const sample of samples) {
    const weight = sample.count || 1
    total += weight

    let current = root
    const stack = [...sample.stack].reverse()
    for (const frame of stack) {
      if (!frame || frame === '0x0') continue
      let child = current.children?.find(c => c.name === frame)
      if (!child) {
        child = { name: frame, value: 0, children: [] }
        if (!current.children) current.children = []
        current.children.push(child)
      }
      child.value += weight
      current = child
    }
  }
  root.value = total
  return root
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
  if (name.includes('[kernel]') || name.startsWith('__')) return '#7c3aed'
  if (name.includes('std::') || name.includes('__cxa')) return '#0d9488'
  if (name.includes('epoll') || name.includes('futex') || name.includes('poll')) return '#b45309'
  const hue = (hashCode(name) % 40) + 10
  const sat = 60 + (depth * 3)
  const lum = 35 + (depth * 2)
  return `hsl(${hue}, ${sat}%, ${lum}%)`
}

function hashCode(s: string): number {
  let h = 0
  for (let i = 0; i < s.length; i++) h = ((h << 5) - h + s.charCodeAt(i)) | 0
  return Math.abs(h)
}
