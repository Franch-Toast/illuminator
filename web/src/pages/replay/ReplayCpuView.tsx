import { colors } from '../../styles/theme'
import { useCpuUtilization, useCpuProcesses } from '../../hooks/useCpuData'
import type { ReplayEngine } from '../../services/replayEngine'
import StackedAreaChart from '../../components/charts/StackedAreaChart'
import CoreHeatmap from '../../components/charts/CoreHeatmap'
import SummaryCards from '../../components/charts/SummaryCards'
import ProcessTable from '../../components/charts/ProcessTable'
import SubTabBar from '../../components/SubTabBar'
import { useState } from 'react'

const SUB_TABS = [
  { id: 'system', label: 'System' },
  { id: 'process', label: 'Process' },
]

export default function ReplayCpuView({ engine }: { engine: ReplayEngine }) {
  const [activeTab, setActiveTab] = useState('system')

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <SubTabBar tabs={SUB_TABS} active={activeTab} onChange={setActiveTab} />

      {activeTab === 'system' && <ReplaySystemSubTab engine={engine} />}
      {activeTab === 'process' && <ReplayProcessSubTab engine={engine} />}
    </div>
  )
}

function ReplaySystemSubTab({ engine }: { engine: ReplayEngine }) {
  const { areaData, coreData, summary } = useCpuUtilization(true, engine)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <SummaryCards data={summary} />

      <div style={{
        background: colors.cardBg, borderRadius: 8, padding: 16,
        border: `1px solid ${colors.cardBorder}`,
      }}>
        <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
          CPU Utilization (Replay)
        </h4>
        <StackedAreaChart data={areaData} group="replay-cpu" />
      </div>

      {coreData.length > 0 && (
        <div style={{
          background: colors.cardBg, borderRadius: 8, padding: 16,
          border: `1px solid ${colors.cardBorder}`,
        }}>
          <h4 style={{ margin: '0 0 8px', fontSize: 13, color: colors.textSecondary }}>
            Per-Core Utilization (Replay)
          </h4>
          <CoreHeatmap data={coreData} />
        </div>
      )}
    </div>
  )
}

function ReplayProcessSubTab({ engine }: { engine: ReplayEngine }) {
  const { processes } = useCpuProcesses(true, engine)

  return (
    <div style={{ marginTop: 8 }}>
      {processes.length === 0 ? (
        <div style={{
          padding: 40, textAlign: 'center', color: colors.textMuted,
          background: colors.cardBg, borderRadius: 8,
          border: `1px solid ${colors.cardBorder}`,
        }}>
          <p style={{ margin: 0, fontSize: 13 }}>
            Press play to start replaying process data
          </p>
        </div>
      ) : (
        <ProcessTable processes={processes} onSelect={() => {}} selectedPid={null} />
      )}
    </div>
  )
}
