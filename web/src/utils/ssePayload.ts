export interface SseRecord {
  labels: Record<string, string>
  fields: Record<string, number | string>
  timestamp?: number
}

interface SsePayload {
  feature?: string
  modelType?: string
  metrics?: SseRecord[]
  records?: SseRecord[]
  samples?: unknown[]
}

/**
 * Extract records from an SSE batch payload.
 * Backend sends time_series data in "metrics" field, trace/generic in "records".
 */
export function extractRecords(payload: unknown): SseRecord[] {
  const p = payload as SsePayload
  return p?.metrics ?? p?.records ?? []
}
