import { create } from 'zustand'

export interface TimeRange {
  start: number
  end: number
}

export type TimeMode = 'live' | 'paused'

interface TimeState {
  mode: TimeMode
  range: TimeRange
  cursor: number | null
  windowMs: number

  setMode: (mode: TimeMode) => void
  togglePause: () => void
  setRange: (range: TimeRange) => void
  setCursor: (ts: number | null) => void
  setWindowMs: (ms: number) => void
  tick: () => void
}

const DEFAULT_WINDOW_MS = 60_000

export const useTimeStore = create<TimeState>((set, get) => ({
  mode: 'live',
  range: {
    start: Date.now() - DEFAULT_WINDOW_MS,
    end: Date.now(),
  },
  cursor: null,
  windowMs: DEFAULT_WINDOW_MS,

  setMode: (mode) => set({ mode }),

  togglePause: () => {
    const { mode } = get()
    if (mode === 'live') {
      set({ mode: 'paused' })
    } else {
      const now = Date.now()
      const { windowMs } = get()
      set({ mode: 'live', range: { start: now - windowMs, end: now }, cursor: null })
    }
  },

  setRange: (range) => set({ range, mode: 'paused' }),

  setCursor: (ts) => set({ cursor: ts }),

  setWindowMs: (ms) => {
    const now = Date.now()
    set({ windowMs: ms, range: { start: now - ms, end: now } })
  },

  tick: () => {
    const { mode, windowMs } = get()
    if (mode === 'live') {
      const now = Date.now()
      set({ range: { start: now - windowMs, end: now } })
    }
  },
}))
