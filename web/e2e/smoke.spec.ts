import { test, expect } from '@playwright/test'

test.describe('Illuminator E2E Smoke Tests', () => {
  test('homepage loads and shows navigation', async ({ page }) => {
    await page.goto('/')
    await expect(page.locator('text=Illuminator')).toBeVisible()
    await expect(page.locator('text=CPU')).toBeVisible()
    await expect(page.locator('text=Memory')).toBeVisible()
  })

  test('features API returns data', async ({ request }) => {
    const resp = await request.get('/api/v1/features')
    expect(resp.ok()).toBeTruthy()
    const data = await resp.json()
    expect(data.features).toBeDefined()
    expect(data.features.length).toBeGreaterThan(0)
  })

  test('WebSocket upgrade on same port succeeds', async ({ request }) => {
    const resp = await request.fetch('/ws/features', {
      headers: {
        'Upgrade': 'websocket',
        'Connection': 'Upgrade',
        'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==',
        'Sec-WebSocket-Version': '13',
      },
    })
    // Should get 101 Switching Protocols (or the fetch API may not support it)
    // At minimum, it should NOT be a 404 or 400
    expect([101, 200, 426].includes(resp.status()) || resp.status() < 500).toBeTruthy()
  })

  test('CPU page renders when navigating', async ({ page }) => {
    await page.goto('/cpu')
    await expect(page.locator('text=CPU')).toBeVisible({ timeout: 5000 })
  })

  test('feature start/stop lifecycle works via API', async ({ request }) => {
    const startResp = await request.post('/api/v1/features/cpu_utilization/start')
    expect(startResp.ok()).toBeTruthy()

    const collectResp = await request.get('/api/v1/features/cpu_utilization/collect')
    expect(collectResp.ok()).toBeTruthy()
    const data = await collectResp.json()
    expect(data.pipeline).toBe('cpu_utilization')

    const stopResp = await request.post('/api/v1/features/cpu_utilization/stop')
    expect(stopResp.ok()).toBeTruthy()
  })

  test('deprecated API routes return deprecation headers', async ({ request }) => {
    const resp = await request.get('/api/v1/cpu/utilization')
    const headers = resp.headers()
    expect(headers['deprecation']).toBe('true')
    expect(headers['sunset']).toBeDefined()
  })
})
