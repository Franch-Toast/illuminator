import { colors } from '../styles/theme'

interface SubTabBarProps {
  tabs: { id: string; label: string }[]
  active: string
  onChange: (id: string) => void
}

export default function SubTabBar({ tabs, active, onChange }: SubTabBarProps) {
  return (
    <div style={{
      display: 'flex', gap: 0, borderBottom: `1px solid ${colors.cardBorder}`,
      marginBottom: 20,
    }}>
      {tabs.map(tab => {
        const isActive = tab.id === active
        return (
          <button
            key={tab.id}
            onClick={() => onChange(tab.id)}
            style={{
              padding: '10px 20px',
              background: 'transparent',
              border: 'none',
              borderBottom: isActive ? `2px solid ${colors.accent}` : '2px solid transparent',
              color: isActive ? colors.accent : colors.textSecondary,
              fontSize: 13,
              fontWeight: isActive ? 600 : 400,
              cursor: 'pointer',
              transition: 'all 0.15s',
            }}
          >
            {tab.label}
          </button>
        )
      })}
    </div>
  )
}
