// ============================================================================
// Illuminator Web 前端入口 — React 18 挂载点
// ============================================================================
//
// main.tsx 是前端应用的入口文件：
// 1. 创建 React 18 的 Concurrent Root（createRoot API）
// 2. 挂载 BrowserRouter 以支持 SPA 路由
// 3. 用 StrictMode 包裹以在开发环境检测潜在问题
// 4. 将整个应用注入到 index.html 中的 <div id="root"> 元素
// ============================================================================

import React from 'react'
import ReactDOM from 'react-dom/client'
import { BrowserRouter } from 'react-router-dom'
import App from './App'

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </React.StrictMode>,
)
