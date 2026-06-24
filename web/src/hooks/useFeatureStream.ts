import { useState, useCallback, useEffect } from 'react'
import { api, FeatureEntry } from '../services/apiClient'

export { TimeSeriesBuffer } from '../utils/timeSeriesBuffer'

export function useFeatureList() {
  const [features, setFeatures] = useState<FeatureEntry[]>([])
  const [loading, setLoading] = useState(true)

  const refresh = useCallback(async () => {
    try {
      const result = await api.features()
      setFeatures(result.features || [])
    } catch {
      // Keep existing state on error
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    refresh() // eslint-disable-line react-hooks/set-state-in-effect
    const timer = setInterval(refresh, 3000)
    return () => clearInterval(timer)
  }, [refresh])

  return { features, loading, refresh }
}
