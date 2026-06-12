import { create } from 'zustand'

export type AnnotationType = 'deploy' | 'alert' | 'manual' | 'spike' | 'gc'

export interface Annotation {
  id: string
  timestamp: number
  type: AnnotationType
  label: string
  description?: string
  color?: string
}

interface AnnotationState {
  annotations: Annotation[]
  add: (ann: Omit<Annotation, 'id'>) => void
  remove: (id: string) => void
  clear: () => void
  getInRange: (start: number, end: number) => Annotation[]
}

let nextId = 1

export const useAnnotationStore = create<AnnotationState>((set, get) => ({
  annotations: [],

  add: (ann) => set(state => ({
    annotations: [...state.annotations, { ...ann, id: `ann-${nextId++}` }],
  })),

  remove: (id) => set(state => ({
    annotations: state.annotations.filter(a => a.id !== id),
  })),

  clear: () => set({ annotations: [] }),

  getInRange: (start, end) => {
    return get().annotations.filter(a => a.timestamp >= start && a.timestamp <= end)
  },
}))

export function getAnnotationColor(type: AnnotationType): string {
  switch (type) {
    case 'deploy': return '#10b981'
    case 'alert': return '#ef4444'
    case 'spike': return '#f59e0b'
    case 'gc': return '#8b5cf6'
    case 'manual': return '#60a5fa'
    default: return '#6b7280'
  }
}

export function getAnnotationIcon(type: AnnotationType): string {
  switch (type) {
    case 'deploy': return '\u{1F680}'
    case 'alert': return '\u26A0'
    case 'spike': return '\u26A1'
    case 'gc': return '\u{1F5D1}'
    case 'manual': return '\u{1F4CC}'
    default: return '\u2022'
  }
}
