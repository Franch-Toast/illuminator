import React, { useEffect, useRef, useState, useCallback } from 'react'
import { flamegraph } from 'd3-flame-graph'
import { select } from 'd3-selection'
import 'd3-flame-graph/dist/d3-flamegraph.css'

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

const selectStyle: React.CSSProperties = {
  background: '#1a1d23', color: '#e0e0e0', border: '1px solid #2a2d35',
  borderRadius: 6, padding: '6px 12px', fontSize: 13,
}

const btnStyle: React.CSSProperties = {
  background: '#2563eb', color: '#fff', border: 'none',
  borderRadius: 6, padding: '6px 14px', fontSize: 13, cursor: 'pointer',
}

const btnActiveStyle: React.CSSProperties = {
  ...btnStyle,
  background: '#1d4ed8',
  boxShadow: 'inset 0 1px 3px rgba(0,0,0,0.4)',
}

interface FetchResult {
  data: FlameNode | null
  stub: boolean
  errorMsg: string
}

async function fetchFlameGraphData(profileType: string): Promise<FetchResult> {
  try {
    // Check pipeline stub status first
    const pipeRes = await fetch('/api/v1/pipelines')
    if (!pipeRes.ok) {
      return { data: null, stub: false, errorMsg: `HTTP ${pipeRes.status}` }
    }
    const pipeData = await pipeRes.json()
    const profilePipeline = (pipeData.pipelines || []).find(
      (p: any) => p.name === (profileType === 'offcpu' ? 'offcpu_analysis' : 'cpu_profile'),
    )
    if (profilePipeline?.stub) {
      return { data: null, stub: true, errorMsg: 'BPF object not configured' }
    }

    const url = profileType === 'offcpu'
      ? '/api/v1/cpu/profile/offcpu'
      : '/api/v1/cpu/profile/flamegraph'
    const res = await fetch(url)
    if (!res.ok) {
      return { data: null, stub: false, errorMsg: `HTTP ${res.status}` }
    }
    const data = await res.json()
    if (data.error) {
      return { data: null, stub: false, errorMsg: data.error }
    }
    const samples = data.stack_samples || []
    if (samples.length > 0) {
      return { data: convertToFlameNode(data, profileType === 'offcpu'), stub: false, errorMsg: '' }
    }
    return { data: null, stub: false, errorMsg: 'No stack samples collected yet' }
  } catch (e) {
    return { data: null, stub: false, errorMsg: e instanceof Error ? e.message : String(e) }
  }
}

function convertToFlameNode(data: any, isOffCpu = false): FlameNode {
  const root: FlameNode = { name: 'root', value: 0, children: [] }
  const samples = data.stack_samples || []

  for (const sample of samples) {
    const frames: string[] = []
    const comm = sample.comm || 'unknown'
    frames.push(comm)
    if (sample.user_stack) {
      for (const f of [...sample.user_stack].reverse()) {
        const name = f.function_name
        if (name && !name.startsWith('[0x'))
          frames.push(name)
        else
          frames.push(`[${comm}]`)
      }
    }
    if (sample.kernel_stack) {
      for (const f of [...sample.kernel_stack].reverse()) {
        frames.push(f.function_name || `[kernel]`)
      }
    }
    if (frames.length === 1) frames.push('[no stack]')
    const weight = isOffCpu && sample.duration_ns
      ? Math.max(1, Math.round(sample.duration_ns / 1000))
      : (sample.count || 1)
    insertStack(root, frames, weight)
  }

  root.value = root.children?.reduce((s, c) => s + c.value, 0) || 0
  return root
}

function insertStack(node: FlameNode, frames: string[], count: number) {
  if (frames.length === 0) return
  const name = frames[0]
  if (!node.children) node.children = []
  let child = node.children.find(c => c.name === name)
  if (!child) {
    child = { name, value: 0, children: [] }
    node.children.push(child)
  }
  child.value += count
  if (frames.length > 1) {
    insertStack(child, frames.slice(1), count)
  }
}


export default function FlameGraph() {
  const chartRef = useRef<HTMLDivElement>(null)
  const [profileType, setProfileType] = useState<'oncpu' | 'offcpu'>('oncpu')
  const [searchText, setSearchText] = useState('')
  const [data, setData] = useState<FlameNode | null>(null)
  const [loading, setLoading] = useState(false)
  const [stub, setStub] = useState(false)
  const [errorMsg, setErrorMsg] = useState('')
  const fgRef = useRef<any>(null)

  const loadData = useCallback(async () => {
    setLoading(true)
    const result = await fetchFlameGraphData(profileType)
    setData(result.data)
    setStub(result.stub)
    setErrorMsg(result.errorMsg)
    setLoading(false)
  }, [profileType])

  useEffect(() => { loadData() }, [loadData])

  useEffect(() => {
    if (!chartRef.current || !data) return
    const el = chartRef.current
    el.innerHTML = ''

    const width = el.clientWidth || 900
    const chart = flamegraph()
      .width(width)
      .cellHeight(18)
      .minFrameSize(1)
      .transitionDuration(300)
      .inverted(true)
      .selfValue(false)

    fgRef.current = chart
    select(el).datum(data).call(chart as any)
  }, [data])

  useEffect(() => {
    if (!fgRef.current) return
    if (searchText) {
      fgRef.current.search(searchText)
    } else {
      fgRef.current.clear()
    }
  }, [searchText])

  const handleExportSvg = () => {
    if (!chartRef.current) return
    const svgEl = chartRef.current.querySelector('svg')
    if (!svgEl) return
    const serializer = new XMLSerializer()
    const source = serializer.serializeToString(svgEl)
    const blob = new Blob([source], { type: 'image/svg+xml' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `flamegraph-${profileType}-${Date.now()}.svg`
    a.click()
    URL.revokeObjectURL(url)
  }

  return (
    <div style={{ padding: 24 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0, fontSize: 22 }}>Flame Graph</h2>
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
          <button
            style={profileType === 'oncpu' ? btnActiveStyle : btnStyle}
            onClick={() => setProfileType('oncpu')}
          >On-CPU</button>
          <button
            style={profileType === 'offcpu' ? btnActiveStyle : btnStyle}
            onClick={() => setProfileType('offcpu')}
          >Off-CPU</button>

          <select style={selectStyle} defaultValue="">
            <option value="">All PIDs</option>
          </select>

          <input
            type="text"
            placeholder="Search functions..."
            value={searchText}
            onChange={e => setSearchText(e.target.value)}
            style={{
              ...selectStyle, width: 180,
            }}
          />

          <button style={btnStyle} onClick={loadData}>
            {loading ? 'Loading...' : 'Refresh'}
          </button>
          <button style={btnStyle} onClick={handleExportSvg}>Export SVG</button>
        </div>
      </div>

      <div style={{
        background: '#1a1d23', border: '1px solid #2a2d35',
        borderRadius: 8, padding: 16, overflowX: 'auto', minHeight: 300,
      }}>
        <div ref={chartRef} />
        {loading && (
          <div style={{ textAlign: 'center', padding: 40, color: '#888' }}>
            Loading profile data...
          </div>
        )}
        {!loading && !data && (
          <div style={{ textAlign: 'center', padding: 60 }}>
            {stub ? (
              <>
                <div style={{ fontSize: 36, marginBottom: 12 }}>🔌</div>
                <div style={{ fontSize: 16, color: '#f59e0b', marginBottom: 8 }}>
                  CPU Profiler is in stub mode
                </div>
                <div style={{ fontSize: 13, color: '#888', maxWidth: 480, margin: '0 auto' }}>
                  The eBPF-based profiler requires a compiled BPF object. Configure <code style={{ color: '#93c5fd' }}>bpf_object</code> path
                  in the pipeline config, or compile probes with <code style={{ color: '#93c5fd' }}>bazel build //src/ebpf/probes:all</code>.
                </div>
              </>
            ) : errorMsg ? (
              <>
                <div style={{ fontSize: 36, marginBottom: 12 }}>📊</div>
                <div style={{ fontSize: 14, color: '#888' }}>
                  {errorMsg}
                </div>
                <div style={{ fontSize: 13, color: '#666', marginTop: 8 }}>
                  Click <strong>Refresh</strong> to retry, or wait for data collection.
                </div>
              </>
            ) : (
              <div style={{ fontSize: 14, color: '#888' }}>
                No profile data available yet. Waiting for samples...
              </div>
            )}
          </div>
        )}
      </div>

      <div style={{ marginTop: 16, display: 'flex', justifyContent: 'space-between', fontSize: 12, color: '#666' }}>
        <span>
          Hover over frames to see details. Click to zoom in. Use search to highlight matching functions.
        </span>
        <span>
          {data ? `${countNodes(data)} unique frames` : ''}
        </span>
      </div>
    </div>
  )
}

function countNodes(node: FlameNode): number {
  let count = 1
  if (node.children) {
    for (const c of node.children) count += countNodes(c)
  }
  return count
}
