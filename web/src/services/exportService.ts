export type ExportFormat = 'csv' | 'json' | 'folded' | 'svg' | 'png'

interface TimeSeriesRecord {
  timestamp: number
  [key: string]: number | string
}

function downloadBlob(blob: Blob, filename: string) {
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
  URL.revokeObjectURL(url)
}

export function exportTimeSeriesCSV(data: TimeSeriesRecord[], filename: string) {
  if (!data.length) return
  const keys = Object.keys(data[0])
  const header = keys.join(',')
  const rows = data.map(row =>
    keys.map(k => {
      const v = row[k]
      if (typeof v === 'string') return `"${v.replace(/"/g, '""')}"`
      return String(v ?? '')
    }).join(',')
  )
  const csv = [header, ...rows].join('\n')
  downloadBlob(new Blob([csv], { type: 'text/csv' }), `${filename}.csv`)
}

export function exportTimeSeriesJSON(data: TimeSeriesRecord[], filename: string) {
  const json = JSON.stringify(data, null, 2)
  downloadBlob(new Blob([json], { type: 'application/json' }), `${filename}.json`)
}

interface FlameNode {
  name: string
  value: number
  children?: FlameNode[]
}

function buildFoldedFromTree(node: FlameNode, path: string[] = []): string[] {
  const lines: string[] = []
  const currentPath = [...path, node.name]
  if (!node.children || node.children.length === 0) {
    lines.push(`${currentPath.join(';')} ${node.value}`)
  } else {
    let childSum = 0
    for (const child of node.children) {
      childSum += child.value
      lines.push(...buildFoldedFromTree(child, currentPath))
    }
    const self = node.value - childSum
    if (self > 0) {
      lines.push(`${currentPath.join(';')} ${self}`)
    }
  }
  return lines
}

export function exportFoldedFormat(root: FlameNode | null, filename: string) {
  if (!root) return
  const lines = buildFoldedFromTree(root)
  downloadBlob(new Blob([lines.join('\n')], { type: 'text/plain' }), `${filename}.folded`)
}

export function exportSVG(svgElement: SVGElement | null, filename: string) {
  if (!svgElement) return
  const serializer = new XMLSerializer()
  const svgStr = serializer.serializeToString(svgElement)
  const blob = new Blob([svgStr], { type: 'image/svg+xml' })
  downloadBlob(blob, `${filename}.svg`)
}

export function exportCanvasAsPNG(canvas: HTMLCanvasElement | null, filename: string) {
  if (!canvas) return
  canvas.toBlob(blob => {
    if (blob) downloadBlob(blob, `${filename}.png`)
  }, 'image/png')
}

export function exportEChartAsPNG(chartContainer: HTMLElement | null, filename: string) {
  if (!chartContainer) return
  const canvas = chartContainer.querySelector('canvas')
  if (canvas) {
    exportCanvasAsPNG(canvas, filename)
  }
}

export function exportCurrentView(filename: string) {
  const mainContent = document.querySelector('main')
  if (!mainContent) return

  const canvas = document.createElement('canvas')
  const rect = mainContent.getBoundingClientRect()
  canvas.width = rect.width * 2
  canvas.height = rect.height * 2
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  import('html2canvas' as string).then((mod) => {
    const html2canvas = mod.default || mod
    html2canvas(mainContent, { scale: 2, backgroundColor: '#0f1117' }).then(
      (resultCanvas: HTMLCanvasElement) => {
        exportCanvasAsPNG(resultCanvas, filename)
      }
    )
  }).catch(() => {
    const allCanvas = mainContent.querySelectorAll('canvas')
    if (allCanvas.length > 0) {
      exportCanvasAsPNG(allCanvas[0] as HTMLCanvasElement, filename)
    }
  })
}
