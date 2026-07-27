import { create } from 'zustand';
import { features as mockFeatures, type Feature, type FeatureStatus } from '../mock/data';

interface FeatureStore {
  features: Feature[];
  selectedFeature: string | null;
  selectFeature: (name: string | null) => void;
  setStatus: (name: string, status: FeatureStatus) => void;
}

export const useFeatureStore = create<FeatureStore>((set) => ({
  features: mockFeatures,
  selectedFeature: null,
  selectFeature: (name) => set({ selectedFeature: name }),
  setStatus: (name, status) =>
    set((state) => ({
      features: state.features.map((f) =>
        f.name === name ? { ...f, status } : f
      ),
    })),
}));
