import { create } from 'zustand';

export type RecordingMode = 'live' | 'recording' | 'replay';

interface RecordingStore {
  mode: RecordingMode;
  recordingStartedAt: number | null;
  recordingElapsed: number;
  replayFileName: string | null;
  replayProgress: number;
  replayDuration: number;
  replaySpeed: number;
  replayPlaying: boolean;

  startRecording: () => void;
  stopRecording: () => void;
  exportRecording: () => void;
  importReplay: (fileName: string, durationSec: number) => void;
  exitReplay: () => void;
  setReplayProgress: (p: number) => void;
  setReplaySpeed: (s: number) => void;
  toggleReplayPlaying: () => void;
  tickRecordingElapsed: () => void;
}

export const useRecordingStore = create<RecordingStore>((set, get) => ({
  mode: 'live',
  recordingStartedAt: null,
  recordingElapsed: 0,
  replayFileName: null,
  replayProgress: 0,
  replayDuration: 0,
  replaySpeed: 1,
  replayPlaying: false,

  startRecording: () =>
    set({ mode: 'recording', recordingStartedAt: Date.now(), recordingElapsed: 0 }),

  stopRecording: () =>
    set({ mode: 'live', recordingStartedAt: null, recordingElapsed: 0 }),

  exportRecording: () => {
    const blob = new Blob(['mock recording data'], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `illuminator-${new Date().toISOString().slice(0, 10)}.ilm`;
    a.click();
    URL.revokeObjectURL(url);
  },

  importReplay: (fileName, durationSec) =>
    set({
      mode: 'replay',
      replayFileName: fileName,
      replayDuration: durationSec,
      replayProgress: 0,
      replaySpeed: 1,
      replayPlaying: false,
    }),

  exitReplay: () =>
    set({
      mode: 'live',
      replayFileName: null,
      replayProgress: 0,
      replayDuration: 0,
      replayPlaying: false,
    }),

  setReplayProgress: (p) => set({ replayProgress: p }),
  setReplaySpeed: (s) => set({ replaySpeed: s }),
  toggleReplayPlaying: () => set((s) => ({ replayPlaying: !s.replayPlaying })),
  tickRecordingElapsed: () => {
    const start = get().recordingStartedAt;
    if (start) set({ recordingElapsed: Math.floor((Date.now() - start) / 1000) });
  },
}));
