/**
 * FeatureConfigPanel — JSON Schema 驱动的自动配置面板
 *
 * 从后端获取 Feature 的 JSON Schema，自动生成配置表单。
 * 用户修改后通过 REST API 实时 reconfigure。
 *
 * 增强功能：
 * - 对 format 为 "pid_list" 的参数，渲染为专用 PID 输入组件
 * - PID 输入支持逗号分隔的数字列表和进程名混合输入
 * - 识别 JSON Schema 中的 "x-requires-restart" 标注并在参数旁提示
 * - 提交后若包含 rodata 参数，提示用户是否需要重启 Feature
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
  success: '#22c55e',
  warning: '#f59e0b',
}

interface SchemaProperty {
  type: string
  default?: unknown
  minimum?: number
  maximum?: number
  items?: { type: string }
  format?: string  // "pid_list" for PID/process name input
  description?: string
  'x-requires-restart'?: boolean
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
  const [initialValues, setInitialValues] = useState<Record<string, unknown>>({})
  const [dirty, setDirty] = useState(false)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [success, setSuccess] = useState(false)
  const [restartMessage, setRestartMessage] = useState<string | null>(null)

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
          const current = await configResp.json()
          setValues(current)
          setInitialValues(current)
          setDirty(false)
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
    setInitialValues(initial)
    setDirty(false)
  }

  const handleChange = (key: string, value: unknown) => {
    setValues(prev => ({ ...prev, [key]: value }))
    setDirty(true)
    setError(null)
    setSuccess(false)
    setRestartMessage(null)
  }

  const restartKeys = schema
    ? Object.entries(schema.properties)
        .filter(([key, prop]) => prop['x-requires-restart'] && values[key] !== initialValues[key])
        .map(([key]) => key)
    : []

  // Reconfigure — 运行时动态更新参数（贯穿 Pipeline 全链路）
  const handleReconfigure = useCallback(async () => {
    setSaving(true)
    setError(null)
    setSuccess(false)
    setRestartMessage(null)
    try {
      const resp = await fetch(`/api/v2/features/${feature}/reconfigure`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(values),
      })
      const data = await resp.json().catch(() => ({ error: `HTTP ${resp.status}` }))
      if (!resp.ok) {
        setError(data.error || `HTTP ${resp.status}`)
      } else {
        setDirty(false)
        setInitialValues({ ...values })
        setSuccess(true)

        if (data.requires_restart || restartKeys.length > 0) {
          const names = restartKeys.length > 0 ? restartKeys : ['部分配置']
          const msg = `参数 ${names.join(', ')} 已保存，需重启 Feature 后生效。`
          setRestartMessage(msg)
          if (window.confirm(`${msg}\n是否立即重启该 Feature？`)) {
            await fetch(`/api/v2/features/${feature}/stop`, { method: 'POST' })
            const startResp = await fetch(`/api/v2/features/${feature}/start`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(values),
            })
            if (!startResp.ok) {
              const startData = await startResp.json().catch(() => ({ error: `HTTP ${startResp.status}` }))
              setError(startData.error || `重启失败: HTTP ${startResp.status}`)
            } else {
              setRestartMessage('Feature 已重启，新配置已生效。')
            }
          }
        }
      }
    } catch (e) {
      setError(String(e))
    } finally {
      setSaving(false)
    }
  }, [feature, values, restartKeys])

  if (!schema || !schema.properties) {
    return null
  }

  const hasPidList = Object.values(schema.properties).some(p => p.format === 'pid_list')

  return (
    <div style={{
      background: colors.surface,
      borderRadius: 8,
      padding: 16,
      border: `1px solid ${colors.border}`,
    }}>
      <h4 style={{ margin: '0 0 12px', fontSize: 14, color: colors.textPrimary }}>
        Configuration
        {hasPidList && (
          <span style={{ marginLeft: 8, fontSize: 11, color: colors.primary }}>
            (PID filter supported)
          </span>
        )}
      </h4>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        {Object.entries(schema.properties).map(([key, prop]) => (
          <div key={key} style={{
            display: 'flex',
            flexDirection: 'column',
            gap: 4,
          }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <label style={{
                width: 160,
                fontSize: 12,
                color: colors.textSecondary,
                fontFamily: 'monospace',
                flexShrink: 0,
              }}>
                {key}
                {schema.required?.includes(key) && <span style={{ color: colors.error }}> *</span>}
                {prop['x-requires-restart'] && (
                  <span style={{ marginLeft: 4, color: colors.warning }} title="需重启生效">↻</span>
                )}
              </label>
              <div style={{ flex: 1 }}>
                {renderInput(key, prop, values[key], handleChange)}
              </div>
            </div>
            {prop.description && (
              <span style={{ fontSize: 11, color: colors.textSecondary, marginLeft: 168 }}>
                {prop.description}
              </span>
            )}
            {prop['x-requires-restart'] && (
              <span style={{ fontSize: 11, color: colors.warning, marginLeft: 168 }}>
                修改后需重启 Feature 生效
              </span>
            )}
          </div>
        ))}
      </div>

      {error && (
        <div style={{ marginTop: 8, fontSize: 12, color: colors.error }}>{error}</div>
      )}
      {success && (
        <div style={{ marginTop: 8, fontSize: 12, color: colors.success }}>
          Reconfigure applied successfully
        </div>
      )}
      {restartMessage && (
        <div style={{ marginTop: 8, fontSize: 12, color: colors.warning }}>
          {restartMessage}
        </div>
      )}

      <div style={{ display: 'flex', gap: 8, marginTop: 12 }}>
        <button
          onClick={handleReconfigure}
          disabled={!dirty || saving}
          style={{
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
          {saving ? 'Applying...' : 'Reconfigure'}
        </button>
      </div>
    </div>
  )
}

function renderInput(
  key: string,
  prop: SchemaProperty,
  value: unknown,
  onChange: (key: string, value: unknown) => void
) {
  const baseStyle = {
    width: '100%',
    padding: '4px 8px',
    border: `1px solid ${colors.border}`,
    borderRadius: 4,
    fontSize: 13,
    background: colors.background,
    color: colors.textPrimary,
    boxSizing: 'border-box' as const,
  }

  // ---- PID list format: 支持数字和进程名混合输入 ----
  if (prop.format === 'pid_list') {
    const isArray = Array.isArray(value)
    const displayValue = isArray
      ? (value as (number | string)[]).map(v => String(v)).join(', ')
      : (typeof value === 'string' ? value : '')

    return (
      <input
        type="text"
        value={displayValue}
        placeholder="e.g. 1234, 5678, nginx, redis-server"
        onChange={(e) => {
          const raw = e.target.value
          // 分割并自动判断数字 vs 进程名
          const parts = raw.split(',').map(s => s.trim()).filter(Boolean)
          const isIntegerItems = prop.items?.type === 'integer'

          if (isIntegerItems) {
            // target_pids: 只接受数字
            const pids = parts
              .map(s => parseInt(s, 10))
              .filter(n => !isNaN(n) && n > 0)
            onChange(key, pids)
          } else {
            // target_process_names: 接受字符串
            onChange(key, parts)
          }
        }}
        style={baseStyle}
      />
    )
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
          style={baseStyle}
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
          style={baseStyle}
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
          style={baseStyle}
        />
      )
    default:
      return <span style={{ fontSize: 12, color: colors.textSecondary }}>unsupported type: {prop.type}</span>
  }
}
