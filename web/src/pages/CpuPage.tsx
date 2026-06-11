import { useState, useCallback, useEffect, useRef } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import FeaturePanel from '../components/FeaturePanel/FeaturePanel'
import { useFeatureStream } from '../hooks/useFeatureStream'
import { useCpuUtilization, useCpuProcesses } from '../hooks/useCpuData'
import { useProcessDetail } from '../hooks/useProcessDetail'
import { api } from '../services/apiClient'
import StackedAreaChart from '../components/charts/StackedAreaChart'
import CoreHeatmap from '../components/charts/CoreHeatmap'
import SummaryCards from '../components/charts/SummaryCards'
import ProcessTable from '../components/charts/ProcessTable'
import ProcessCpuTimeline from '../components/charts/ProcessCpuTimeline'
import ProfileSnapshot from '../components/charts/ProfileSnapshot'
import ThreadBreakdown from '../components/charts/ThreadBreakdown'

const SUB_TABS = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function CpuPage() {
  const [activeTab, setActiveTab] = useState('system')

  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column' }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 20, color: colors.textPrimary }}>CPU</h2>
      <SubTabBar tabs={SUB_TABS} active={activeTab} onChange={setActiveTab} />

      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const util = useFeatureStream('cpu_utilization', { pollIntervalMs: 1000 })
  const { areaData, coreData, summary, clear } = useCpuUtilization(util.state === 'active')

  const handleStop = useCallback(() => {
    util.stop()
    clear()
  }, [util, clear])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <FeaturePanel
        featureName="CPU Utilization"
        state={util.state}
        error={util.error}
        onStart={util.start}
        onStop={handleStop}
        onPause={util.pause}
        onResume={util.resume}
        isRecording={util.isRecording}
        onRecordToggle={util.toggleRecording}
      >
        <SummaryCards data={summary} />
        <div style={{ marginTop: 16 }}>
          <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
            System CPU Utilization (Stacked)
          </h4>
          <StackedAreaChart data={areaData} width={700} height={200} />
        </div>
        <div style={{ marginTop: 16 }}>
          <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
            Per-Core Utilization
          </h4>
          <CoreHeatmap data={coreData} width={700} />
        </div>
      </FeaturePanel>
    </div>
  )
}

function ProcessSubTab() {
  const proc = useFeatureStream('cpu_processes', { pollIntervalMs: 2000 })
  const { processes, clear } = useCpuProcesses(proc.state === 'active')
  const [selectedPid, setSelectedPid] = useState<number | null>(null)

  const handleStop = useCallback(() => {
    proc.stop()
    clear()
  }, [proc, clear])

  const selectedProcess = processes.find(p => p.pid === selectedPid)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <FeaturePanel
        featureName="Process CPU (Top-N)"
        state={proc.state}
        error={proc.error}
        onStart={proc.start}
        onStop={handleStop}
        onPause={proc.pause}
        onResume={proc.resume}
        isRecording={proc.isRecording}
        onRecordToggle={proc.toggleRecording}
      >
        {!selectedPid ? (
          <ProcessTable
            processes={processes}
            onSelect={setSelectedPid}
            selectedPid={selectedPid}
          />
        ) : (
          <ProcessDetailView
            pid={selectedPid}
            comm={selectedProcess?.comm ?? '?'}
            onBack={() => setSelectedPid(null)}
          />
        )}
      </FeaturePanel>
    </div>
  )
}

function ProcessDetailView({ pid, comm, onBack }: { pid: number; comm: string; onBack: () => void }) {
  const [profileType, setProfileType] = useState<'on_cpu' | 'off_cpu'>('on_cpu')
  const [selectedTimestamp, setSelectedTimestamp] = useState<number | null>(null)
  const [profilingStatus, setProfilingStatus] = useState<'starting' | 'active' | 'failed'>('starting')
  const { timeline, threads } = useProcessDetail(pid, true)
  const startedRef = useRef(false)

  useEffect(() => {
    if (startedRef.current) return
    startedRef.current = true

    const ensureFeatures = async () => {
      try {
        const resp = await api.features()
        const cpuProf = resp.features?.find((f: { name: string }) => f.name === 'cpu_profile')
        const offcpuProf = resp.features?.find((f: { name: string }) => f.name === 'offcpu_profile')

        const starts: Promise<unknown>[] = []
        if (cpuProf && cpuProf.state === 'inactive') starts.push(api.featureStart('cpu_profile'))
        if (offcpuProf && offcpuProf.state === 'inactive') starts.push(api.featureStart('offcpu_profile'))

        if (starts.length > 0) await Promise.all(starts)
        setProfilingStatus('active')
      } catch {
        setProfilingStatus('failed')
      }
    }
    ensureFeatures()
  }, [pid])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      {/* Breadcrumb */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <button
          onClick={onBack}
          style={{
            background: 'none', border: 'none', color: colors.accent,
            cursor: 'pointer', fontSize: 13, padding: 0,
          }}
        >
          &larr; Process List
        </button>
        <span style={{ color: colors.textMuted }}>/</span>
        <span style={{ fontSize: 14, fontWeight: 600, color: colors.textPrimary }}>
          {comm} <span style={{ fontWeight: 400, color: colors.textMuted }}>(PID:{pid})</span>
        </span>
      </div>

      {/* CPU Loading Timeline */}
      <div style={{
        background: colors.bg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          CPU Loading Timeline
          <span style={{ fontWeight: 400, color: colors.textMuted, marginLeft: 8 }}>
            (click to select time point)
          </span>
        </h4>
        <ProcessCpuTimeline
          data={timeline}
          selectedTimestamp={selectedTimestamp}
          onTimeSelect={setSelectedTimestamp}
        />
      </div>

      {/* Profile Type Selector */}
      <div style={{ display: 'flex', gap: 8 }}>
        <ProfileTypeButton
          label="On-CPU"
          active={profileType === 'on_cpu'}
          onClick={() => setProfileType('on_cpu')}
        />
        <ProfileTypeButton
          label="Off-CPU"
          active={profileType === 'off_cpu'}
          onClick={() => setProfileType('off_cpu')}
        />
        <span style={{ flex: 1 }} />
        <span style={{ fontSize: 12, color: colors.textMuted, alignSelf: 'center' }}>
          Target: {comm} (PID:{pid})
        </span>
      </div>

      {/* Profile Snapshot (Flamegraph) */}
      <div style={{
        background: colors.bg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        {profilingStatus === 'starting' && (
          <div style={{ padding: 16, textAlign: 'center', color: colors.textMuted, fontSize: 12 }}>
            Starting profiling engines...
          </div>
        )}
        {profilingStatus === 'failed' && (
          <div style={{ padding: 16, textAlign: 'center', color: colors.danger, fontSize: 12 }}>
            Failed to start profiling features. eBPF may not be available.
          </div>
        )}
        {profilingStatus === 'active' && (
          <ProfileSnapshot
            pid={pid}
            comm={comm}
            profileType={profileType}
            selectedTimestamp={null}
            threadComms={threads.map(t => t.comm)}
          />
        )}
      </div>

      {/* Thread Breakdown */}
      <div style={{
        background: colors.bg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Thread Breakdown
        </h4>
        <ThreadBreakdown threads={threads} processComm={comm} />
      </div>
    </div>
  )
}

function ProfileTypeButton({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      style={{
        padding: '6px 14px', borderRadius: 4, fontSize: 12, fontWeight: 500,
        border: `1px solid ${active ? colors.accent : colors.cardBorder}`,
        background: active ? colors.activeBg : 'transparent',
        color: active ? colors.accent : colors.textSecondary,
        cursor: 'pointer',
      }}
    >
      {active ? '◉' : '○'} {label}
    </button>
  )
}

