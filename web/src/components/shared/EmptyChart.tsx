import { colors } from '../../styles/theme'

interface EmptyChartProps {
  message: string
  height?: number
}

export default function EmptyChart({ message, height = 100 }: EmptyChartProps) {
  return (
    <div style={{
      height, display: 'flex', alignItems: 'center', justifyContent: 'center',
      color: colors.textMuted, fontSize: 12,
    }}>
      {message}
    </div>
  )
}
