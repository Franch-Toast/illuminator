import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    port: 3000,
    proxy: {
      '/api': 'http://localhost:9527',
      '/healthz': 'http://localhost:9527',
      '/metrics': 'http://localhost:9527',
      '/ws': { target: 'ws://localhost:9527', ws: true },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
})
