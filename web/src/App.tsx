// ============================================================================
// Illuminator Web 前端 — 应用路由和导航布局
// ============================================================================
//
// App.tsx 是 Illuminator Web 前端的根组件，负责：
// 1. 定义整体布局（左侧导航栏 + 右侧主内容区）
// 2. 配置所有页面的路由规则
// 3. 管理深色主题的 UI 样式
//
// 页面路由：
// ==========
// /            → CpuOverview     — CPU 利用率仪表盘（面积图 + 核心热力图）
// /processes   → ProcessExplorer — 进程浏览器（可排序表格 + 线程展开）
// /flamegraph  → FlameGraph      — 火焰图（d3-flame-graph 可视化 + 搜索）
// /timeline    → Timeline        — 调度器分析（运行队列延迟 + 迁移统计）
// /diff        → DiffView        — 对比分析（基准 vs 比较，变化百分比）
// /query       → QueryConsole   — SQL 查询控制台（直查 SQLite 存储）
// ============================================================================

import React from 'react'
import { Routes, Route, NavLink } from 'react-router-dom'
import CpuOverview from './pages/CpuOverview'
import ProcessExplorer from './pages/ProcessExplorer'
import FlameGraph from './pages/FlameGraph'
import Timeline from './pages/Timeline'
import DiffView from './pages/DiffView'
import QueryConsole from './pages/QueryConsole'

// 导航项配置：路径、标签和图标
const navItems = [
  { path: '/', label: 'CPU Overview', icon: '📊' },
  { path: '/processes', label: 'Processes', icon: '📋' },
  { path: '/flamegraph', label: 'Flame Graph', icon: '🔥' },
  { path: '/timeline', label: 'Timeline', icon: '📈' },
  { path: '/diff', label: 'Diff View', icon: '🔀' },
  { path: '/query', label: 'Query', icon: '🔍' },
]

export default function App() {
  return (
    // 根布局：左侧导航 + 右侧内容
    <div style={{ display: 'flex', height: '100vh', fontFamily: '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif' }}>

      {/* ======== 左侧导航栏 ======== */}
      <nav style={{
        width: 220, background: '#1a1d23', color: '#e0e0e0',
        display: 'flex', flexDirection: 'column', padding: '16px 0',
        borderRight: '1px solid #2a2d35',
      }}>
        {/* 品牌标识 */}
        <div style={{
          padding: '0 20px 20px', borderBottom: '1px solid #2a2d35',
          marginBottom: 8
        }}>
          <h1 style={{ fontSize: 20, margin: 0, color: '#60a5fa', fontWeight: 700 }}>
            Illuminator
          </h1>
          <span style={{ fontSize: 11, color: '#888' }}>Observability Platform</span>
        </div>

        {/* 导航链接 — NavLink 自动高亮当前路由 */}
        {navItems.map(item => (
          <NavLink
            key={item.path}
            to={item.path}
            end={item.path === '/'}  // 精确匹配首页路径
            style={({ isActive }) => ({
              display: 'flex', alignItems: 'center', gap: 10,
              padding: '10px 20px', textDecoration: 'none',
              color: isActive ? '#60a5fa' : '#b0b0b0',
              background: isActive ? '#252830' : 'transparent',
              borderLeft: isActive ? '3px solid #60a5fa' : '3px solid transparent',
              fontSize: 14, transition: 'all 0.15s',
            })}
          >
            <span>{item.icon}</span>
            <span>{item.label}</span>
          </NavLink>
        ))}

        {/* 底部版本信息 */}
        <div style={{ flex: 1 }} />
        <div style={{ padding: '12px 20px', fontSize: 11, color: '#666' }}>
          v0.1.0 · C++ eBPF Engine
        </div>
      </nav>

      {/* ======== 右侧主内容区 ======== */}
      <main style={{ flex: 1, background: '#0f1117', color: '#e0e0e0', overflow: 'auto' }}>
        <Routes>
          <Route path="/" element={<CpuOverview />} />
          <Route path="/processes" element={<ProcessExplorer />} />
          <Route path="/flamegraph" element={<FlameGraph />} />
          <Route path="/timeline" element={<Timeline />} />
          <Route path="/diff" element={<DiffView />} />
          <Route path="/query" element={<QueryConsole />} />
        </Routes>
      </main>
    </div>
  )
}
