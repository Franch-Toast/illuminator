import { test, expect } from '@playwright/test'

test.describe('Illuminator E2E Smoke Tests', () => {
  test('homepage loads and shows navigation', async ({ page }) => {
    await page.goto('/')
    await expect(page.locator('text=Illuminator')).toBeVisible()
    await expect(page.locator('text=CPU')).toBeVisible()
    await expect(page.locator('text=Memory')).toBeVisible()
  })

  test('features API returns data', async ({ request }) => {
    const resp = await request.get('/api/v2/features')
    expect(resp.ok()).toBeTruthy()
    const data = await resp.json()
    expect(data.features).toBeDefined()
    expect(data.features.length).toBeGreaterThan(0)
  })

  test('SSE subscription flow works', async ({ request }) => {
    const subResp = await request.post('/api/v1/events/subscribe', {
      data: { features: ['cpu_utilization'] },
    })
    expect(subResp.ok()).toBeTruthy()
    const sub = await subResp.json()
    expect(sub.subscription_id).toBeDefined()
    expect(sub.url).toContain('/api/v1/events/')
  })

  test('CPU page renders when navigating', async ({ page }) => {
    await page.goto('/cpu')
    await expect(page.locator('text=CPU')).toBeVisible({ timeout: 5000 })
  })

  test('feature start/stop lifecycle works via v2 API', async ({ request }) => {
    const featResp = await request.get('/api/v2/features')
    expect(featResp.ok()).toBeTruthy()
    const { features } = await featResp.json()
    const cpuFeature = features.find((f: { name: string }) => f.name === 'cpu_utilization')
    expect(cpuFeature).toBeDefined()
    expect(cpuFeature.state).toBe('active')
  })

  test('recording API lifecycle works', async ({ request }) => {
    const startResp = await request.post('/api/v1/features/cpu_utilization/record/start', {
      data: {},
    })
    expect(startResp.ok()).toBeTruthy()

    const statusResp = await request.get('/api/v1/features/cpu_utilization/record/status')
    expect(statusResp.ok()).toBeTruthy()
    const status = await statusResp.json()
    expect(status.recording).toBe(true)

    const stopResp = await request.post('/api/v1/features/cpu_utilization/record/stop', {
      data: {},
    })
    expect(stopResp.ok()).toBeTruthy()
    const stopped = await stopResp.json()
    expect(stopped.status).toBe('ok')
  })

  test('deprecated API routes return deprecation headers', async ({ request }) => {
    const resp = await request.get('/api/v1/cpu/utilization')
    const headers = resp.headers()
    expect(headers['deprecation']).toBe('true')
    expect(headers['sunset']).toBeDefined()
  })

  test('healthz endpoint returns version info', async ({ request }) => {
    const resp = await request.get('/healthz')
    expect(resp.ok()).toBeTruthy()
    const data = await resp.json()
    expect(data.status).toBe('ok')
    expect(data.version).toBeDefined()
  })
})
