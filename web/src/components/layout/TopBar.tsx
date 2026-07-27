import { useEffect, useRef, useState } from 'react';
import {
  Radio,
  Circle,
  Square,
  Download,
  Upload,
  ChevronDown,
  SkipBack,
  Play,
  Pause as PauseIcon,
  SkipForward,
  X,
} from 'lucide-react';
import clsx from 'clsx';
import { useTimeStore, type TimeRange } from '../../stores/time-store';
import { useConnectionStore } from '../../stores/connection-store';
import { useRecordingStore } from '../../stores/recording-store';

const ranges: TimeRange[] = ['1m', '5m', '15m', '30m', '1h'];

export function TopBar() {
  const { range, setRange } = useTimeStore();
  const connected = useConnectionStore((s) => s.connected);
  const { mode, recordingElapsed, replayFileName, replayProgress, replayDuration, replaySpeed, replayPlaying } =
    useRecordingStore();

  return (
    <header className="h-14 flex items-center gap-4 px-5 border-b border-border bg-surface/80 backdrop-blur-sm flex-shrink-0">
      {mode === 'replay' ? (
        <ReplayControls />
      ) : (
        <div className="flex items-center gap-1 bg-surface-2 rounded-radius-sm p-0.5">
          {ranges.map((r) => (
            <button
              key={r}
              onClick={() => setRange(r)}
              className={clsx(
                'px-3 py-1 text-xs font-medium rounded-md transition-colors cursor-pointer',
                range === r
                  ? 'bg-accent text-white'
                  : 'text-text-secondary hover:text-text-primary'
              )}
            >
              {r}
            </button>
          ))}
        </div>
      )}

      <div className="flex-1" />

      {/* SSE Status */}
      <div className="flex items-center gap-2 text-xs text-text-secondary">
        <span
          className={clsx(
            'w-2 h-2 rounded-full',
            connected ? 'bg-status-active' : 'bg-status-error'
          )}
          style={connected ? { animation: 'pulse-glow 2s ease-in-out infinite', color: '#22c55e' } : undefined}
        />
        <span>{connected ? 'Connected' : 'Disconnected'}</span>
      </div>

      {/* Record controls */}
      {mode !== 'replay' && <RecordDropdown />}

      {/* Live / Recording indicator */}
      <div className="flex items-center gap-1.5">
        {mode === 'recording' ? (
          <span className="flex items-center gap-1.5 text-status-error text-xs font-semibold">
            <span className="w-2 h-2 rounded-full bg-status-error" style={{ animation: 'rec-pulse 1.5s infinite' }} />
            REC {formatSec(recordingElapsed)}
          </span>
        ) : mode === 'replay' ? (
          <span className="text-xs text-amber-400 font-medium">{replayFileName}</span>
        ) : (
          <span className="flex items-center gap-1.5 text-xs text-status-active font-medium">
            <Radio size={14} />
            Live
          </span>
        )}
      </div>
    </header>
  );
}

function RecordDropdown() {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  const { mode, startRecording, stopRecording, exportRecording, importReplay } = useRecordingStore();

  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, []);

  const handleImport = () => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.ilm';
    input.onchange = () => {
      const file = input.files?.[0];
      if (file) importReplay(file.name, 300);
    };
    input.click();
    setOpen(false);
  };

  return (
    <div ref={ref} className="relative">
      <button
        onClick={() => setOpen(!open)}
        className={clsx(
          'flex items-center gap-1.5 px-2.5 py-1.5 rounded-radius-sm text-xs transition-colors cursor-pointer border',
          mode === 'recording'
            ? 'border-status-error/40 bg-status-error/10 text-status-error'
            : 'border-border hover:border-accent/30 text-text-secondary hover:text-text-primary'
        )}
      >
        <Circle size={12} fill={mode === 'recording' ? 'currentColor' : 'none'} />
        Record
        <ChevronDown size={12} />
      </button>

      {open && (
        <div className="absolute right-0 top-full mt-1 w-48 bg-surface border border-border rounded-radius-sm shadow-lg z-50 py-1">
          {mode !== 'recording' ? (
            <DropItem icon={<Circle size={13} />} label="Start Recording" onClick={() => { startRecording(); setOpen(false); }} />
          ) : (
            <DropItem icon={<Square size={13} />} label="Stop Recording" onClick={() => { stopRecording(); setOpen(false); }} />
          )}
          <div className="border-t border-border my-1" />
          <DropItem icon={<Download size={13} />} label="Export (.ilm)" onClick={() => { exportRecording(); setOpen(false); }} />
          <DropItem icon={<Upload size={13} />} label="Import (.ilm)" onClick={handleImport} />
        </div>
      )}
    </div>
  );
}

function DropItem({ icon, label, onClick }: { icon: React.ReactNode; label: string; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className="w-full flex items-center gap-2.5 px-3 py-2 text-xs text-text-secondary hover:bg-surface-2 hover:text-text-primary transition-colors cursor-pointer"
    >
      {icon}
      {label}
    </button>
  );
}

function ReplayControls() {
  const { replayProgress, replayDuration, replaySpeed, replayPlaying, setReplayProgress, setReplaySpeed, toggleReplayPlaying, exitReplay } =
    useRecordingStore();

  const speeds = [0.5, 1, 2, 4];

  return (
    <div className="flex items-center gap-3">
      <button onClick={() => setReplayProgress(0)} className="p-1.5 text-text-secondary hover:text-text-primary cursor-pointer">
        <SkipBack size={15} />
      </button>
      <button onClick={toggleReplayPlaying} className="p-1.5 text-text-secondary hover:text-text-primary cursor-pointer">
        {replayPlaying ? <PauseIcon size={15} /> : <Play size={15} />}
      </button>
      <button onClick={() => setReplayProgress(replayDuration)} className="p-1.5 text-text-secondary hover:text-text-primary cursor-pointer">
        <SkipForward size={15} />
      </button>

      <input
        type="range"
        min={0}
        max={replayDuration}
        value={replayProgress}
        onChange={(e) => setReplayProgress(Number(e.target.value))}
        className="w-40 accent-accent h-1 cursor-pointer"
      />
      <span className="text-xs font-mono text-text-muted w-24 text-center">
        {formatSec(replayProgress)} / {formatSec(replayDuration)}
      </span>

      <select
        value={replaySpeed}
        onChange={(e) => setReplaySpeed(Number(e.target.value))}
        className="bg-surface-2 border border-border rounded px-2 py-1 text-xs text-text-secondary cursor-pointer"
      >
        {speeds.map((s) => (
          <option key={s} value={s}>{s}x</option>
        ))}
      </select>

      <button onClick={exitReplay} className="p-1.5 text-text-muted hover:text-status-error cursor-pointer" title="Exit replay">
        <X size={15} />
      </button>
    </div>
  );
}

function formatSec(sec: number): string {
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return `${String(m).padStart(2, '0')}:${String(Math.floor(s)).padStart(2, '0')}`;
}
