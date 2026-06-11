import { colors } from '../styles/theme'
import FeaturePanel from '../components/FeaturePanel/FeaturePanel'
import { useFeatureStream } from '../hooks/useFeatureStream'
import CpuOverview from './CpuOverview'
import ProcessExplorer from './ProcessExplorer'
import FlameGraph from './FlameGraph'
import Timeline from './Timeline'

function CpuUtilizationPanel() {
  const { state, error, start, stop, pause, resume, isRecording, toggleRecording } = useFeatureStream('cpu_utilization')

  return (
    <FeaturePanel
      featureName="CPU Utilization"
      state={state}
      error={error}
      onStart={start}
      onStop={stop}
      onPause={pause}
      onResume={resume}
      isRecording={isRecording}
      onRecordToggle={toggleRecording}
    >
      <CpuOverview />
    </FeaturePanel>
  )
}

function ProcessCpuPanel() {
  const { state, error, start, stop, pause, resume, isRecording, toggleRecording } = useFeatureStream('cpu_processes')

  return (
    <FeaturePanel
      featureName="Process CPU (Top-N)"
      state={state}
      error={error}
      onStart={start}
      onStop={stop}
      onPause={pause}
      onResume={resume}
      isRecording={isRecording}
      onRecordToggle={toggleRecording}
    >
      <ProcessExplorer />
    </FeaturePanel>
  )
}

function CpuProfilePanel() {
  const { state, error, start, stop, pause, resume, isRecording, toggleRecording } = useFeatureStream('cpu_profile')

  return (
    <FeaturePanel
      featureName="CPU Profile (On-CPU Flamegraph)"
      state={state}
      error={error}
      onStart={start}
      onStop={stop}
      onPause={pause}
      onResume={resume}
      isRecording={isRecording}
      onRecordToggle={toggleRecording}
    >
      <FlameGraph />
    </FeaturePanel>
  )
}

function OffCpuPanel() {
  const { state, error, start, stop, pause, resume, isRecording, toggleRecording } = useFeatureStream('offcpu_profile')

  return (
    <FeaturePanel
      featureName="Off-CPU Analysis"
      state={state}
      error={error}
      onStart={start}
      onStop={stop}
      onPause={pause}
      onResume={resume}
      isRecording={isRecording}
      onRecordToggle={toggleRecording}
    >
      <FlameGraph />
    </FeaturePanel>
  )
}

function SchedulerPanel() {
  const { state, error, start, stop, pause, resume, isRecording, toggleRecording } = useFeatureStream('sched_analysis')

  return (
    <FeaturePanel
      featureName="Scheduler Analysis"
      state={state}
      error={error}
      onStart={start}
      onStop={stop}
      onPause={pause}
      onResume={resume}
      isRecording={isRecording}
      onRecordToggle={toggleRecording}
    >
      <Timeline />
    </FeaturePanel>
  )
}

export default function CpuPage() {
  return (
    <div style={{ padding: 24, display: 'flex', flexDirection: 'column', gap: 20 }}>
      <h2 style={{ margin: 0, fontSize: 20, color: colors.textPrimary }}>CPU</h2>
      <CpuUtilizationPanel />
      <ProcessCpuPanel />
      <CpuProfilePanel />
      <OffCpuPanel />
      <SchedulerPanel />
    </div>
  )
}
