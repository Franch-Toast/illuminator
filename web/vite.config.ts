import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { execSync } from 'child_process'

function getGitVersion(): string {
  if (process.env.VITE_APP_VERSION) return process.env.VITE_APP_VERSION
  try {
    const tag = execSync('git describe --tags --exact-match 2>/dev/null', { encoding: 'utf-8' }).trim()
    return tag.replace(/^v/, '')
  } catch {
    try {
      return execSync('git describe --tags --always 2>/dev/null', { encoding: 'utf-8' }).trim()
    } catch {
      try {
        return execSync('git rev-parse --short=8 HEAD', { encoding: 'utf-8' }).trim()
      } catch {
        return 'dev'
      }
    }
  }
}

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: {
    __APP_VERSION__: JSON.stringify(getGitVersion()),
  },
  server: {
    port: 3000,
    hmr: {
      path: '/__vite_hmr',
    },
    proxy: {
      '/api': 'http://localhost:9527',
      '/healthz': 'http://localhost:9527',
      '/metrics': 'http://localhost:9527',
      '/ws': {
        target: 'http://localhost:9527',
        ws: true,
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
    rollupOptions: {
      output: {
        manualChunks: {
          echarts: ['echarts/core', 'echarts/charts', 'echarts/components', 'echarts/renderers'],
          vendor: ['react', 'react-dom', 'react-router-dom'],
        },
      },
    },
  },
})
