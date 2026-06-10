import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
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
  plugins: [react()],
  define: {
    __APP_VERSION__: JSON.stringify(getGitVersion()),
  },
  server: {
    port: 3000,
    proxy: {
      '/api': 'http://localhost:9527',
      '/healthz': 'http://localhost:9527',
      '/metrics': 'http://localhost:9527',
      '/ws': { target: 'ws://localhost:9528', ws: true },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
})
