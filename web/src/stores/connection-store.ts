import { create } from 'zustand';

interface ConnectionStore {
  connected: boolean;
  latency: number;
  messagesPerSec: number;
  activeFeatures: number;
  setConnected: (v: boolean) => void;
  setLatency: (v: number) => void;
  setMessagesPerSec: (v: number) => void;
  setActiveFeatures: (v: number) => void;
}

export const useConnectionStore = create<ConnectionStore>((set) => ({
  connected: true,
  latency: 0.8,
  messagesPerSec: 42,
  activeFeatures: 5,
  setConnected: (connected) => set({ connected }),
  setLatency: (latency) => set({ latency }),
  setMessagesPerSec: (messagesPerSec) => set({ messagesPerSec }),
  setActiveFeatures: (activeFeatures) => set({ activeFeatures }),
}));
