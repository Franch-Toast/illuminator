import { create } from 'zustand';

interface UIStore {
  sidebarCollapsed: boolean;
  toggleSidebar: () => void;
  setSidebarCollapsed: (v: boolean) => void;
}

const saved = typeof localStorage !== 'undefined'
  ? localStorage.getItem('illuminator-sidebar-collapsed')
  : null;

export const useUIStore = create<UIStore>((set) => ({
  sidebarCollapsed: saved === 'true',
  toggleSidebar: () =>
    set((s) => {
      const next = !s.sidebarCollapsed;
      localStorage.setItem('illuminator-sidebar-collapsed', String(next));
      return { sidebarCollapsed: next };
    }),
  setSidebarCollapsed: (v) => {
    localStorage.setItem('illuminator-sidebar-collapsed', String(v));
    set({ sidebarCollapsed: v });
  },
}));
