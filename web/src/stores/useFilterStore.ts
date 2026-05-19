import { create } from 'zustand'

interface FilterState {
  pid: number | null
  comm: string | null
  cpu: number | null

  setPid: (pid: number | null) => void
  setComm: (comm: string | null) => void
  setCpu: (cpu: number | null) => void
  clearAll: () => void
}

export const useFilterStore = create<FilterState>((set) => ({
  pid: null,
  comm: null,
  cpu: null,

  setPid: (pid) => set({ pid }),
  setComm: (comm) => set({ comm }),
  setCpu: (cpu) => set({ cpu }),
  clearAll: () => set({ pid: null, comm: null, cpu: null }),
}))
