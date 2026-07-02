/**
 * FeatureConfigPanel — JSON Schema 驱动的自动配置面板
 *
 * 从后端获取 Feature 的 JSON Schema，自动生成配置表单。
 * 用户修改后通过 REST API 实时 reconfigure。
 */

import { useState, useEffect, useCallback } from 'react'
const colors = {
  surface: '#1a1d23',
  border: '#2a2d35',
  background: '#0f1117',
  textPrimary: '#e0e0e0',
  textSecondary: '#b0b0b0',
  primary: '#60a5fa',
  error: '#ef4444',
}

interface SchemaProperty {
  type: string
  default?: unknown
  minimum?: number
  maximum?: number
  items?: { type: string }
}

interface JsonSchema {
  type: string
  properties: Record<string, SchemaProperty>
  required?: string[]
}

interface Props {
  feature: string
  schema?: string
}

export default function FeatureConfigPanel({ feature, schema: schemaProp }: Props) {
  const [schema, setSchema] = useState<JsonSchema | null>(null)
  const [values, setValues] = useState<Record<string, unknown>>({})
  const [dirty, setDirty] = useState(false)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (schemaProp) {
      try {
        const parsed = JSON.parse(schemaProp)
        setSchema(parsed)
        initValues(parsed)
      } catch { /* invalid schema */ }
    } else {
      fetchSchema()
    }
  }, [feature, schemaProp])

  const fetchSchema = async () => {
    try {
      const [schemaResp, configResp] = await Promise.all([
        fetch(`/api/v2/features/${feature}/config/schema`),
        fetch(`/api/v2/features/${feature}/config`),
      ])
      if (schemaResp.ok) {
        const data = await schemaResp.json()
        setSchema(data)
        if (configResp.ok) {
          setValues(await configResp.json())
        } else {
          initValues(data)
        }
      }
    } catch { /* ignore */ }
  }

  const initValues = (s: JsonSchema) => {
    const initial: Record<string, unknown> = {}
    for (const [key, prop] of Object.entries(s.properties || {})) {
      if (prop.default !== undefined) initial[key] = prop.default
    }
    setValues(initial)
  }

  const handleChange = (key: string, value: unknown) => {
    setValues(prev => ({ ...prev, [key]: value }))
    setDirty(true)
    setError(null)
  }

  const handleApply = useCallback(async () => {
    setSaving(true)
    setError(null)
    try {
      const resp = await fetch(`/api/v2/features/${feature}/config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(values),
      })
      if (!resp.ok) {
        const data = await resp.json()
        setError(data.error || `HTTP ${resp.status}`)
      } else {
        setDirty(false)
      }
    } catch (e) {
      setError(String(e))
    } finally {
      setSaving(false)
    }
  }, [feature, values])

  if (!schema || !schema.properties) {
    return null
  }

  return (
    <div style={{
      background: colors.surface,
      borderRadius: 8,
      padding: 16,
      border: `1px solid ${colors.border}`,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 14, color: colors.textPrimary }}>
        Configuration
      </h4>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        {Object.entries(schema.properties).map(([key, prop]) => (
          <div key={key} style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <label style={{
              width: 140,
              fontSize: 12,
              color: colors.textSecondary,
              fontFamily: 'monospace',
            }}>
              {key}
              {schema.required?.includes(key) && <span style={{ color: colors.error }}> *</span>}
            </label>
            {renderInput(key, prop, values[key], handleChange)}
          </div>
        ))}
      </div>

      {error && (
        <div style={{ marginTop: 8, fontSize: 12, color: colors.error }}>{error}</div>
      )}

      <button
        onClick={handleApply}
        disabled={!dirty || saving}
        style={{
          marginTop: 12,
          padding: '6px 16px',
          border: 'none',
          borderRadius: 6,
          cursor: dirty && !saving ? 'pointer' : 'not-allowed',
          fontSize: 13,
          fontWeight: 500,
          background: dirty ? colors.primary : colors.border,
          color: '#fff',
          opacity: dirty && !saving ? 1 : 0.5,
        }}
      >
        {saving ? 'Applying...' : 'Apply'}
      </button>
    </div>
  )
}

function renderInput(
  key: string,
  prop: SchemaProperty,
  value: unknown,
  onChange: (key: string, value: unknown) => void
) {
  const style = {
    flex: 1,
    padding: '4px 8px',
    border: `1px solid ${colors.border}`,
    borderRadius: 4,
    fontSize: 13,
    background: colors.background,
    color: colors.textPrimary,
  }

  switch (prop.type) {
    case 'integer':
    case 'number':
      return (
        <input
          type="number"
          value={value as number ?? prop.default ?? 0}
          min={prop.minimum}
          max={prop.maximum}
          step={prop.type === 'integer' ? 1 : 0.1}
          onChange={(e) => onChange(key, prop.type === 'integer' ? parseInt(e.target.value) : parseFloat(e.target.value))}
          style={style}
        />
      )
    case 'boolean':
      return (
        <input
          type="checkbox"
          checked={value as boolean ?? prop.default ?? false}
          onChange={(e) => onChange(key, e.target.checked)}
        />
      )
    case 'string':
      return (
        <input
          type="text"
          value={value as string ?? prop.default ?? ''}
          onChange={(e) => onChange(key, e.target.value)}
          style={style}
        />
      )
    case 'array':
      return (
        <input
          type="text"
          value={Array.isArray(value) ? (value as number[]).join(', ') : ''}
          placeholder="comma-separated values"
          onChange={(e) => {
            const raw = e.target.value
            if (prop.items?.type === 'integer') {
              onChange(key, raw.split(',').map(s => parseInt(s.trim())).filter(n => !isNaN(n)))
            } else {
              onChange(key, raw.split(',').map(s => s.trim()).filter(Boolean))
            }
          }}
          style={style}
        />
      )
    default:
      return <span style={{ fontSize: 12, color: colors.textSecondary }}>unsupported type: {prop.type}</span>
  }
}
