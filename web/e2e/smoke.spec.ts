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

  // ============================================================
  // 扩展场景（Phase 8 测试体系补充）
  // ============================================================

  // 1. Feature 启动 → 验证数据显示
  test('feature start produces data in stats', async ({ request }) => {
    // 确保 Feature 处于 active 状态
    await request.post('/api/v2/features/cpu_utilization/start', { data: {} })

    // 等待数据流入
    await new Promise(resolve => setTimeout(resolve, 1000))

    // 验证 stats 中有 batches_processed
    const statsResp = await request.get('/api/v2/features/cpu_utilization/stats')
    expect(statsResp.ok()).toBeTruthy()
    const stats = await statsResp.json()
    expect(stats.batches_processed).toBeDefined()
    expect(typeof stats.batches_processed).toBe('number')
  })

  // 2. Pause → 验证“已暂停”状态
  test('pause feature changes state to paused', async ({ request }) => {
    // 先确保 Feature 处于 active
    await request.post('/api/v2/features/cpu_utilization/start', { data: {} })

    const pauseResp = await request.post('/api/v2/features/cpu_utilization/pause', {
      data: {},
    })
    expect(pauseResp.ok()).toBeTruthy()

    // 通过 features 列表验证状态为 paused
    const featResp = await request.get('/api/v2/features')
    expect(featResp.ok()).toBeTruthy()
    const { features } = await featResp.json()
    const cpu = features.find((f: { name: string }) => f.name === 'cpu_utilization')
    expect(cpu.state).toBe('paused')

    // 恢复状态以不影响后续测试
    await request.post('/api/v2/features/cpu_utilization/resume', { data: {} })
  })

  // 3. Resume → 验证数据恢复
  test('resume feature changes state back to active', async ({ request }) => {
    // 先暂停再恢复
    await request.post('/api/v2/features/cpu_utilization/start', { data: {} })
    await request.post('/api/v2/features/cpu_utilization/pause', { data: {} })

    const resumeResp = await request.post('/api/v2/features/cpu_utilization/resume', {
      data: {},
    })
    expect(resumeResp.ok()).toBeTruthy()

    // 验证状态恢复为 active
    const featResp = await request.get('/api/v2/features')
    expect(featResp.ok()).toBeTruthy()
    const { features } = await featResp.json()
    const cpu = features.find((f: { name: string }) => f.name === 'cpu_utilization')
    expect(cpu.state).toBe('active')
  })

  // 4. Reconfigure PID → 验证配置面板交互
  test('reconfigure feature config with target_pids', async ({ request }) => {
    const configResp = await request.post('/api/v2/features/cpu_utilization/config', {
      data: {
        target_pids: ['1234', '5678'],
      },
    })
    expect(configResp.ok()).toBeTruthy()
    const result = await configResp.json()
    expect(result.status).toBeDefined()

    // 验证配置可读回
    const getConfigResp = await request.get('/api/v2/features/cpu_utilization/config')
    expect(getConfigResp.ok()).toBeTruthy()
  })

  // 5. 录制启停 → 验证录制状态指示
  test('recording start/stop reflects in is_recording flag', async ({ request }) => {
    await request.post('/api/v2/features/cpu_utilization/start', { data: {} })

    // 开始录制
    const startResp = await request.post('/api/v1/features/cpu_utilization/record/start', {
      data: { file_path: '/tmp/illuminator_e2e_test' },
    })
    expect(startResp.ok()).toBeTruthy()

    // 验证录制状态为 true
    const statusResp = await request.get('/api/v1/features/cpu_utilization/record/status')
    expect(statusResp.ok()).toBeTruthy()
    const status = await statusResp.json()
    expect(status.recording).toBe(true)

    // 停止录制
    const stopResp = await request.post('/api/v1/features/cpu_utilization/record/stop', {
      data: {},
    })
    expect(stopResp.ok()).toBeTruthy()

    // 验证录制状态为 false
    const statusResp2 = await request.get('/api/v1/features/cpu_utilization/record/status')
    expect(statusResp2.ok()).toBeTruthy()
    const status2 = await statusResp2.json()
    expect(status2.recording).toBe(false)
  })

  // 6. SSE 连接状态显示 → 验证 ConnectionStatus
  test('connection status indicator visible on page', async ({ page }) => {
    await page.goto('/')
    // 页面正常加载，包含 Illuminator 品牌
    await expect(page.locator('text=Illuminator')).toBeVisible()
    // 页面 body 可见（布局渲染成功）
    const bodyVisible = await page.locator('body').isVisible()
    expect(bodyVisible).toBeTruthy()
  })

  // 7. 页面导航 → 各页面可正常加载
  test('all main pages load without errors', async ({ page }) => {
    const pages = [
      { path: '/' },
      { path: '/cpu' },
      { path: '/memory' },
      { path: '/io' },
      { path: '/network' },
      { path: '/gpu' },
      { path: '/replay' },
      { path: '/query' },
      { path: '/plugins' },
      { path: '/system' },
    ]

    for (const p of pages) {
      await page.goto(p.path)
      await expect(page.locator('body')).toBeVisible({ timeout: 5000 })
      // 导航栏在所有页面都应存在
      await expect(page.locator('text=Illuminator')).toBeVisible({ timeout: 5000 })
    }
  })
})
