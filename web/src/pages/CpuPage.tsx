import { useState, useRef } from 'react'
import { colors } from '../styles/theme'
import SubTabBar from '../components/SubTabBar'
import { useCpuUtilization, useCpuProcesses } from '../hooks/useCpuData'
import { useProcessDetail } from '../hooks/useProcessDetail'
import { useUrlState } from '../hooks/useUrlState'
import { api } from '../services/apiClient'
import StackedAreaChart from '../components/charts/StackedAreaChart'
import CoreHeatmap from '../components/charts/CoreHeatmap'
import SummaryCards from '../components/charts/SummaryCards'
import ProcessTable from '../components/charts/ProcessTable'
import ProcessCpuTimeline from '../components/charts/ProcessCpuTimeline'
import ProfileSnapshot from '../components/charts/ProfileSnapshot'
import ThreadBreakdown from '../components/charts/ThreadBreakdown'
import FeatureHealthBadge from '../components/FeatureHealthBadge'

const SUB_TABS = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function CpuPage() {
  const { subTab, setUrlState } = useUrlState()
  const activeTab = subTab || 'system'

  const handleTabChange = (tab: string) => {
    setUrlState({ subTab: tab })
  }

  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column' }}>
      <h2 style={{ margin: '0 0 16px', fontSize: 20, color: colors.textPrimary, display: 'flex', alignItems: 'center', gap: 8 }}>
        CPU <FeatureHealthBadge featureName="cpu_utilization" />
      </h2>
      <SubTabBar tabs={SUB_TABS} active={activeTab} onChange={handleTabChange} />

      {activeTab === 'system' && <SystemSubTab />}
      {activeTab === 'process' && <ProcessSubTab />}
    </div>
  )
}

function SystemSubTab() {
  const { areaData, coreData, summary } = useCpuUtilization(true)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, marginTop: 16 }}>
      <SummaryCards data={summary} />

      <div style={{
        background: colors.cardBg, borderRadius: 8, padding: 16,
        border: `1px solid ${colors.cardBorder}`,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          System CPU Utilization
        </h4>
        <StackedAreaChart data={areaData} group="cpu-charts" />
      </div>

      <div style={{
        background: colors.cardBg, borderRadius: 8, padding: 16,
        border: `1px solid ${colors.cardBorder}`,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Per-Core Utilization
        </h4>
        <CoreHeatmap data={coreData} />
      </div>
    </div>
  )
}

function ProcessSubTab() {
  const { processes } = useCpuProcesses(true)
  const { pid: urlPid, comm: urlComm, setUrlState } = useUrlState()
  const [selectedPid, setSelectedPid] = useState<number | null>(urlPid)
  const [selectedComm, setSelectedComm] = useState(urlComm || '')

  const handleSelect = (pid: number) => {
    const proc = processes.find(p => p.pid === pid)
    if (proc) {
      setSelectedPid(pid)
      setSelectedComm(proc.comm)
      setUrlState({ pid, comm: proc.comm })
    }
  }

  const handleBack = () => {
    setSelectedPid(null)
    setSelectedComm('')
    setUrlState({ pid: null, comm: null, profileType: null })
  }

  if (selectedPid) {
    return (
      <ProcessDetailView
        pid={selectedPid}
        comm={selectedComm}
        onBack={handleBack}
      />
    )
  }

  return (
    <div style={{ marginTop: 16 }}>
      <ProcessTable
        processes={processes}
        onSelect={handleSelect}
        selectedPid={selectedPid}
      />
    </div>
  )
}

function ProcessDetailView({ pid, comm, onBack }: { pid: number; comm: string; onBack: () => void }) {
  const [profileType, setProfileType] = useState<'on_cpu' | 'off_cpu'>('on_cpu')
  const [profilingStatus, setProfilingStatus] = useState<'idle' | 'starting' | 'active' | 'failed'>('idle')
  const [timeSelection, setTimeSelection] = useState<import('../components/charts/ProfileSnapshot').TimeSelection | null>(null)
  const { timeline, threads, processGone } = useProcessDetail(pid, true)
  const startedRef = useRef(false)

  const startProfiling = async () => {
    if (startedRef.current) return
    startedRef.current = true
    setProfilingStatus('starting')
    try {
      const profilerConfig = { target_pids: [pid] }
      await Promise.all([
        api.featureStart('cpu_profiler', profilerConfig),
        api.featureStart('offcpu_profiler', profilerConfig),
      ])
      setProfilingStatus('active')
    } catch {
      setProfilingStatus('failed')
      startedRef.current = false
    }
  }

  const stopProfiling = async () => {
    try {
      await Promise.all([
        api.featureStop('cpu_profiler'),
        api.featureStop('offcpu_profiler'),
      ])
    } catch { /* best effort */ }
    setProfilingStatus('idle')
    startedRef.current = false
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, marginTop: 16 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <button onClick={onBack} style={{
          background: 'none', border: 'none', color: colors.accent,
          cursor: 'pointer', fontSize: 13, padding: 0,
        }}>
          &larr; Process List
        </button>
        <span style={{ color: colors.textMuted }}>/</span>
        <span style={{ fontSize: 14, fontWeight: 600, color: colors.textPrimary }}>
          {comm} <span style={{ fontWeight: 400, color: colors.textMuted }}>(PID:{pid})</span>
        </span>
      </div>

      {/* Process gone banner */}
      {processGone && (
        <div style={{
          background: 'rgba(251,191,36,0.08)', border: '1px solid rgba(251,191,36,0.3)',
          borderRadius: 8, padding: '10px 16px', display: 'flex', alignItems: 'center', gap: 8,
        }}>
          <span style={{ fontSize: 14 }}>&#9888;</span>
          <span style={{ fontSize: 12, color: '#fbbf24' }}>
            Process {comm} (PID:{pid}) has dropped below top-N CPU threshold. Timeline continues with 0% readings.
            Profiling data already collected is preserved.
          </span>
        </div>
      )}

      {/* Always-On section header */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <span style={{ fontSize: 10, fontWeight: 600, color: '#4ade80', textTransform: 'uppercase', letterSpacing: 1 }}>
          Always-On Monitoring
        </span>
        <span style={{ flex: 1, height: 1, background: 'rgba(74,222,128,0.2)' }} />
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          CPU Timeline
        </h4>
        <ProcessCpuTimeline data={timeline} selectedTimestamp={null} onTimeSelect={setTimeSelection} />
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          Thread Breakdown ({threads.length} threads)
        </h4>
        <div style={{ maxHeight: 300, overflowY: 'auto' }}>
          <ThreadBreakdown threads={threads} processComm={comm} />
        </div>
      </div>

      {/* On-Demand section header */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 8 }}>
        <span style={{ fontSize: 10, fontWeight: 600, color: '#f59e0b', textTransform: 'uppercase', letterSpacing: 1 }}>
          On-Demand Profiling
        </span>
        <span style={{ flex: 1, height: 1, background: 'rgba(245,158,11,0.2)' }} />
        <span style={{ fontSize: 9, color: colors.textMuted }}>Session-based · ~3% CPU overhead</span>
      </div>

      <div style={{
        background: colors.cardBg, border: `1px solid ${colors.cardBorder}`,
        borderRadius: 8, padding: 16,
      }}>
        {profilingStatus === 'idle' && (
          <div style={{ textAlign: 'center', padding: 20 }}>
            <p style={{ margin: '0 0 12px', fontSize: 13, color: colors.textSecondary }}>
              CPU sampling analysis for this process. Generates flame graphs.
            </p>
            <p style={{ margin: '0 0 16px', fontSize: 11, color: colors.textMuted }}>
              Sampling freq: 49Hz | Est. overhead: ~3% CPU
            </p>
            <button onClick={startProfiling} style={{
              padding: '8px 20px', borderRadius: 6, border: 'none',
              background: '#2563eb', color: '#fff', fontSize: 13, cursor: 'pointer',
            }}>
              Start CPU Profile
            </button>
          </div>
        )}
        {profilingStatus === 'starting' && (
          <div style={{ padding: 16, textAlign: 'center', color: colors.textMuted, fontSize: 12 }}>
            Starting profiling engines...
          </div>
        )}
        {profilingStatus === 'failed' && (
          <div style={{ padding: 16, textAlign: 'center' }}>
            <p style={{ color: colors.danger, fontSize: 12, margin: '0 0 8px' }}>
              Failed to start profiling. eBPF may not be available.
            </p>
            <button onClick={() => { startedRef.current = false; startProfiling() }} style={{
              padding: '6px 14px', borderRadius: 4, border: `1px solid ${colors.danger}`,
              background: 'transparent', color: colors.danger, fontSize: 12, cursor: 'pointer',
            }}>
              Retry
            </button>
          </div>
        )}
        {profilingStatus === 'active' && (
          <>
            <div style={{ display: 'flex', gap: 8, marginBottom: 12, alignItems: 'center' }}>
              <ProfileTypeButton label="On-CPU" active={profileType === 'on_cpu'}
                onClick={() => setProfileType('on_cpu')} />
              <ProfileTypeButton label="Off-CPU" active={profileType === 'off_cpu'}
                onClick={() => setProfileType('off_cpu')} />
              <div style={{ flex: 1 }} />
              <button onClick={stopProfiling} style={{
                padding: '5px 12px', borderRadius: 4, fontSize: 11, fontWeight: 500,
                border: '1px solid rgba(239,68,68,0.4)', background: 'rgba(239,68,68,0.08)',
                color: '#ef4444', cursor: 'pointer',
              }}>
                Stop Session
              </button>
            </div>
            <ProfileSnapshot
              pid={pid}
              comm={comm}
              profileType={profileType}
              selectedTimestamp={null}
              timeSelection={timeSelection}
              threadComms={threads.map(t => t.comm)}
            />
          </>
        )}
      </div>
    </div>
  )
}

function ProfileTypeButton({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      padding: '6px 14px', borderRadius: 4, fontSize: 12, fontWeight: 500,
      border: `1px solid ${active ? colors.accent : colors.cardBorder}`,
      background: active ? colors.activeBg : 'transparent',
      color: active ? colors.accent : colors.textSecondary,
      cursor: 'pointer',
    }}>
      {active ? '◉' : '○'} {label}
    </button>
  )
}
