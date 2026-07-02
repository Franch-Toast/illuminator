import { useState, useCallback, useEffect } from 'react'
import { api, FeatureDescriptor } from '../services/apiClient'

export { TimeSeriesBuffer } from '../utils/timeSeriesBuffer'

export function useFeatureList() {
  const [features, setFeatures] = useState<FeatureDescriptor[]>([])
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
    refresh()
    const timer = setInterval(refresh, 5000)
    return () => clearInterval(timer)
  }, [refresh])

  return { features, loading, refresh }
}
