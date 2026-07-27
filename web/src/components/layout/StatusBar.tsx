import { useConnectionStore } from '../../stores/connection-store';
import { useRecordingStore } from '../../stores/recording-store';

export function StatusBar() {
  const { messagesPerSec, latency, activeFeatures } = useConnectionStore();
  const { mode, replayFileName } = useRecordingStore();

  return (
    <footer className="h-8 flex items-center justify-between px-5 border-t border-border bg-surface text-xs text-text-muted flex-shrink-0 font-mono">
      <div className="flex items-center gap-4">
        {mode === 'replay' ? (
          <>
            <span className="text-amber-400 font-medium">Replay Mode</span>
            <span>{replayFileName}</span>
          </>
        ) : mode === 'recording' ? (
          <>
            <span className="text-status-error font-medium" style={{ animation: 'rec-pulse 1.5s infinite' }}>
              Recording...
            </span>
            <Metric label="msg/s" value={messagesPerSec} />
          </>
        ) : (
          <>
            <Metric label="msg/s" value={messagesPerSec} />
            <Metric label="latency" value={`${latency}ms`} />
            <Metric label="features" value={activeFeatures} />
          </>
        )}
      </div>

      <span className="text-text-muted">v{__APP_VERSION__}</span>
    </footer>
  );
}

function Metric({ label, value }: { label: string; value: string | number }) {
  return (
    <span>
      <span className="text-text-muted">{label}: </span>
      <span className="text-text-secondary">{value}</span>
    </span>
  );
}
