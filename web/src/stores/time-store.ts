import { create } from 'zustand';

export type TimeRange = '1m' | '5m' | '15m' | '30m' | '1h';

interface TimeStore {
  range: TimeRange;
  setRange: (r: TimeRange) => void;
}

export const useTimeStore = create<TimeStore>((set) => ({
  range: '5m',
  setRange: (range) => set({ range }),
}));
