import { defineConfig } from '@playwright/test'

export default defineConfig({
  testDir: './e2e',
  timeout: 30_000,
  retries: 1,
  use: {
    baseURL: 'http://127.0.0.1:9527',
    headless: true,
    screenshot: 'only-on-failure',
  },
  webServer: {
    command: 'echo "Using external daemon on port 9527"',
    url: 'http://127.0.0.1:9527/healthz',
    reuseExistingServer: true,
    timeout: 5000,
  },
  projects: [
    { name: 'chromium', use: { browserName: 'chromium' } },
  ],
})
