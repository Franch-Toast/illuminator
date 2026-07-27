# Illuminator 前端设计方案 v3

> 状态：全面重新设计  
> 设计系统：由 `ui-ux-pro-max` + `ui-styling` + `design-system` skill 驱动生成  
> 参考项目：Grafana、Pyroscope、Netdata、Perfetto、speedscope  
> 风格定位：Real-Time Monitoring + Data-Dense Dashboard + Dark Mode (OLED)

---

## 一、产品定位与设计原则

### 1.1 产品定位

Illuminator 是一个 **高自由度、插件化、可录制回放** 的深度性能分析平台：

- **高自由度**：用户可自由组合 Feature（CPU/Memory/I/O/Network/Scheduler），每个 Feature 独立可控
- **插件化**：支持动态加载/卸载 .so 插件，配置由 schema 驱动自动生成 UI
- **可录制回放**：支持将实时 SSE 数据录制为快照，事后回放分析

### 1.2 设计原则（由 skill 验证）

| 原则 | 来源 | 说明 |
|------|------|------|
| **问题驱动** | Grafana | 每个面板回答一个具体问题，而非展示数据 |
| **零配置** | Netdata | 启动即可用，自动发现 Feature 并生成 dashboard |
| **渐进式细节** | Grafana + Perfetto | Overview → Feature → Deep Dive 三级钻取 |
| **60fps 交互** | speedscope | 火焰图使用 WebGL 渲染，100MB+ profile 不卡顿 |
| **信息密度优先** | ui-ux-pro-max (density=8) | 用 8-12px gap 和紧凑排版最大化数据可见性 |
| **颜色仅表示语义** | Grafana | 颜色仅表示状态和严重性，不做装饰 |
| **可访问性 AA** | ui-ux-pro-max checklist | 文本对比度 ≥4.5:1，焦点状态可见，支持 `prefers-reduced-motion` |

---

## 二、设计系统（Design Tokens）

### 2.1 Token 架构（三层结构）

遵循 `design-system` skill 的 Primitive → Semantic → Component 三层架构：

```
Primitive (原始值)   →   Semantic (语义别名)   →   Component (组件级)
--blue-600: #4F46E5       --color-accent           --btn-primary-bg
--gray-900: #111118       --color-surface           --card-bg
```

### 2.2 色彩系统

| Token | Light Mode | Dark Mode (默认) | 用途 |
|-------|-----------|------------------|------|
| `--color-bg-deep` | `#F8FAFC` | `#08080d` | 最底层背景 |
| `--color-surface` | `#FFFFFF` | `#111118` | 卡片/面板背景 |
| `--color-surface-2` | `#F1F5F9` | `#191922` | 输入框/按钮背景 |
| `--color-surface-3` | `#E2E8F0` | `#22222e` | Hover 状态背景 |
| `--color-border` | `#DBEAFE` | `#2a2a3a` | 边框 |
| `--color-text-primary` | `#1E3A8A` | `#eeeef5` | 主要文本 |
| `--color-text-secondary` | `#475569` | `#a0a0b8` | 次要文本 |
| `--color-text-muted` | `#94A3B8` | `#6b6b82` | 辅助文本 |
| `--color-accent` | `#4F46E5` | `#6366f1` | 品牌强调色（Indigo） |
| `--color-accent-light` | `#6366F1` | `#818cf8` | 浅强调色 |
| `--color-accent-dim` | `#3730A3` | `#4f46e5` | 深强调色 |

### 2.3 状态色（语义固定，跨主题不变）

| 状态 | 颜色 | Token | 用法 |
|------|------|-------|------|
| Active | `#22C55E` | `--color-status-active` | Feature 运行中 |
| Paused | `#EAB308` | `--color-status-paused` | Feature 暂停 |
| Inactive | `#6B7280` | `--color-status-inactive` | Feature 未启动 |
| Error | `#EF4444` | `--color-status-error` | 错误/Critical 阈值 |
| Warning | `#F59E0B` | `--color-status-warning` | Warning 阈值 |
| Diff 增加 | `#EF4444` | `--color-diff-add` | Pyroscope 风格：增加=红 |
| Diff 减少 | `#22C55E` | `--color-diff-remove` | Pyroscope 风格：减少=绿 |

### 2.4 字体系统

由 `ui-ux-pro-max --design-system` 推荐：

| 角色 | 字体 | 回退 | 用法 |
|------|------|------|------|
| **Display / Body** | `Inter` | `-apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif` | 所有 UI 文本 |
| **Monospace / Code** | `JetBrains Mono` | `'Fira Code', 'Cascadia Code', monospace` | 数值、代码、表格数据 |

**排版规范（Data-Dense 模式）**：

| 元素 | 尺寸 | 字重 | 行高 |
|------|------|------|------|
| 页面标题 (h1) | 24px | 700 (Bold) | 1.2 |
| 板块标题 (h2) | 18px | 600 (Semibold) | 1.3 |
| 卡片标题 | 14-15px | 600 | 1.4 |
| 正文 | 14px | 400 | 1.5 |
| 辅助文本 | 12px | 400/500 | 1.4 |
| KPI 大数字 | 32-48px (clamp) | 700 | 1.1 |
| 表格数据 | 13px (mono) | 400 | 1.3 |
| 状态标签 | 11-12px | 500-600 | 1.0 |

### 2.5 间距系统（8dp 节奏，Dense 模式）

| Token | 值 | 用法 |
|-------|-----|------|
| `--space-1` | 4px | 图标与文本间距 |
| `--space-2` | 8px | 紧凑组件内部间距 |
| `--space-3` | 12px | 表格行内 padding |
| `--space-4` | 16px | 卡片内 padding |
| `--space-5` | 20px | 组件间 gap |
| `--space-6` | 24px | 板块间 gap |
| `--space-8` | 32px | 页面级 padding（小屏） |
| `--space-10` | clamp(24px, 3vw, 48px) | 页面级 padding（响应式） |

### 2.6 圆角与阴影

| Token | 值 | 用法 |
|-------|-----|------|
| `--radius-sm` | 8px | 按钮、输入框 |
| `--radius-md` | 12px | 卡片 |
| `--radius-lg` | 16px | 面板、Dialog |
| `--radius-xl` | 20px | 大面板 |
| `--shadow-card` | `0 1px 3px rgba(0,0,0,0.1)` | 卡片悬浮 |
| `--shadow-glow` | `0 0 0 1px accent/12, 0 12px 40px -12px accent/10` | 卡片 hover glow |

---

## 三、技术选型

| 层级 | 选择 | 理由 |
|------|------|------|
| **框架** | React 18+ TypeScript | 组件化、类型安全、生态成熟 |
| **构建** | Vite 5+ | 极速 HMR，已有配置 |
| **组件库** | shadcn/ui (按需) + Tailwind CSS 4 | 参考 `ui-styling` skill：Radix UI 无障碍基础 + Utility-first 样式 |
| **状态管理** | Zustand | 轻量，与 SSE stream 契合 |
| **时序图表** | ECharts (按需 tree-shaking) | 支持热力图/时序图/直方图 |
| **火焰图** | 自研 WebGL + Canvas 双层 | 参考 speedscope：WebGL 绘矩形 + Canvas 2D 绘文字 |
| **路由** | React Router v6+ | 声明式路由 |
| **图标** | Lucide React | 统一 SVG 图标，一致的线宽和风格 |
| **代码分割** | `React.lazy()` + `Suspense` | 参考 React stack skill：路由和重型组件 lazy load |
| **虚拟列表** | `@tanstack/react-virtual` | 参考 React stack skill：>100 项列表必须虚拟化 |
| **动画** | CSS Keyframes + `useTransition` | 参考 ui-ux-pro-max (motion=6)：标准 stagger 动画 |

---

## 四、整体布局

### 4.1 主框架结构

```
┌──────────────────────────────────────────────────────────────┐
│  TopBar (h-14, 56px)                                          │
│  [TimeRange] ─────────────────── [SSE Status] [Record] [Live] │
├──────────┬───────────────────────────────────────────────────┤
│          │                                                    │
│ Sidebar  │  Main Content Area                                 │
│ (260px)  │  padding: clamp(24px, 3vw, 48px)                  │
│          │                                                    │
│ collapse │  ┌─────────────────────────────────────────────┐   │
│ at 900px │  │  页面内容（无 max-width 限制）                │   │
│ → 64px   │  │  Grid 响应式：                                │   │
│          │  │    sm: 1→2 col                                │   │
│ ● Overv. │  │    lg: 2→3 col                                │   │
│ ─────    │  │    2xl: 3→4 col                               │   │
│   CPU    │  └─────────────────────────────────────────────┘   │
│   Memory │                                                    │
│   I/O    │                                                    │
│   Network│                                                    │
│   Sched  │                                                    │
│ ─────    │                                                    │
│   Flame  │                                                    │
│   Plugin │                                                    │
│          │                                                    │
├──────────┴───────────────────────────────────────────────────┤
│ StatusBar (h-8, 32px)  msg/s │ latency │ features │ version   │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 响应式断点策略

参考 `tailwind-responsive.md` 的 mobile-first 原则：

| 断点 | 视口宽度 | 布局行为 |
|------|---------|---------|
| 默认 (mobile) | < 640px | 1 列，侧边栏隐藏，TopBar 简化 |
| `sm` | ≥ 640px | 2 列 Grid |
| `md` | ≥ 768px | 侧边栏 overlay 模式 |
| `lg` | ≥ 1024px | 侧边栏固定展开，2 列 Feature 卡 |
| `xl` | ≥ 1280px | 3 列 Feature 卡 |
| `2xl` | ≥ 1536px | 4 列金信号，3 列 Feature 卡，侧面板展开 |

### 4.3 侧边栏规格

| 属性 | 展开状态 | 折叠状态 |
|------|---------|---------|
| 宽度 | 260px | 64px |
| 导航项高度 | 44px (≥44pt touch target) | 44px |
| 图标尺寸 | 19px | 19px |
| 文本尺寸 | 14px | 隐藏 |
| 内边距 | px-4 py-3 (mx-3) | px-0 py-3 (mx-2 居中) |
| 折叠触发 | 手动点击 | 自动折叠 ≤ 900px |
| 导航分组 | 显示分组标题 (11px uppercase) | 显示分隔线 |
| 活跃指示 | 左侧 3px accent 竖条 + 背景高亮 | 仅背景高亮 |

### 4.4 TopBar 规格

| 元素 | 规格 |
|------|------|
| 高度 | 56px (h-14) |
| 背景 | surface/80 + backdrop-blur-sm |
| 时间范围选择器 | 按钮组：1m / 5m / 15m / 30m / 1h，选中态 accent 背景 |
| SSE 状态指示 | 2.5px 圆点 + 文字，connected=green + pulse-glow 动画 |
| Record 按钮 | 录制中=error 红色脉冲，未录制=surface-2 边框 |
| Live 指示器 | Radio 图标 + "Live" 文字 |

### 4.5 StatusBar 规格

| 元素 | 规格 |
|------|------|
| 高度 | 32px (h-8) |
| 内容 | `msg/s` / `latency` / `features active` / `version` |
| 字体 | 12px，text-muted |

---

## 五、页面设计

### 5.1 Overview — 四大金信号 + Feature 卡片

**参考**：Grafana SRE Dashboard + Netdata 自动发现 + ui-ux-pro-max (Executive Dashboard)

```
┌──────────────────────────────────────────────────────────────┐
│ System Overview                                               │
│ Golden Signals at a glance — real-time system health          │
│                                                               │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐          │
│ │  延迟      │ │ 吞吐量    │ │  错误率    │ │  饱和度    │          │
│ │  2.3ms    │ │  1.2k/s  │ │  0.01%   │ │  45%     │          │
│ │  ▁▂▃▃▂▂▁  │ │  ▃▃▄▅▅▄▃  │ │  ▁▁▁▁▁▁▁  │ │  ▃▃▄▄▄▅▅  │          │
│ └──────────┘ └──────────┘ └──────────┘ └──────────┘          │
│                                                               │
│ Feature Status                                                │
│ Auto-discovered · 5 active · 7 total                         │
│                                                               │
│ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐           │
│ │🟢 CPU Util    │ │🟡 CPU Prof    │ │⚫ Off-CPU     │           │
│ │ 78% avg      │ │ paused       │ │ inactive     │           │
│ │ ▅▆▆▇▇▇▆▅     │ │ 0 samples    │ │ -            │           │
│ │[Pause][Stop]  │ │[Resume][Stop] │ │[Start]       │           │
│ │        [Cfg] │ │        [Cfg] │ │       [Cfg]  │           │
│ └──────────────┘ └──────────────┘ └──────────────┘           │
└──────────────────────────────────────────────────────────────┘
```

**StatCard 规格**：
- 内边距：`p-6` (24px)
- 标签：12px uppercase tracking-wide，text-muted
- 数值：32-48px bold，CountUp 动画 (easeOutQuart, 1200ms)
- Sparkline：36px 高，SVG path，区域填充 8% 透明度
- 趋势图标：TrendingUp / TrendingDown / Minus
- Hover：`card-glow` 效果

**FeatureCard 规格**：
- 内边距：`p-6` (24px)
- 状态指示：2.5px 圆点，active 态 pulse-glow 动画
- 名称：16px semibold
- 数值：24px bold
- Sparkline：40px 高
- 操作栏：顶部 border-t 分隔，Pause/Resume/Stop/Config 按钮
- Tier 标签：12px rounded-full badge (monitoring=blue, profiling=purple, tracing=orange)

**Grid 断点**：
- 金信号行：`grid-cols-1 sm:grid-cols-2 2xl:grid-cols-4`
- Feature 卡片：`grid-cols-1 lg:grid-cols-2 2xl:grid-cols-3`
- Gap：24px (gap-6)

### 5.2 Feature 详情页（通用模板）

所有 Feature 共用一个模板，根据 `modelType` 渲染不同主视图：

```
┌──────────────────────────────────────────────────────────────┐
│ [Feature 名] [STATUS] 78% avg 1000ms                         │
│ ────────────────────── [Pause] [Configure] [Record] [Reset]  │
│                                                               │
│ ┌──────────────────────────┐ ┌──────────────────────┐        │
│ │ 主图表区                   │ │ 辅助面板               │        │
│ │                           │ │                       │        │
│ │ time_series → ECharts     │ │ [进程][线程][核心] tab │        │
│ │   (area chart, 360px h)  │ │                       │        │
│ │                           │ │ PID  CMD   CPU%  MEM │        │
│ │ profile → WebGL FlameGraph│ │ 1234 nginx 45%  128M │        │
│ │                           │ │ 5678 node  22%  256M │        │
│ │ trace → Timeline          │ │ ...                  │        │
│ │                           │ │ (虚拟滚动列表)        │        │
│ └──────────────────────────┘ └──────────────────────┘        │
│                                                               │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ [Raw Data] [BPF Stats] [Configuration]                    │ │
│ │ { "feature": "cpu_utilization", ... }                     │ │
│ └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

**布局规格**：
- 主图表 + 辅助面板：`grid-cols-1 2xl:grid-cols-[1fr_420px]`
- 底部 Tab 面板：全宽，可折叠
- 辅助面板表格：固定 grid `grid-cols-[60px_1fr_64px_76px]`

### 5.3 火焰图页（参考 speedscope + Pyroscope）

**五种视图模式**：

| 模式 | 参考 | 说明 |
|------|------|------|
| **Flame** | speedscope | WebGL 渲染火焰图，滚轮缩放/拖拽平移 |
| **Table** | Pyroscope | 函数表，按 self/total time 排序 |
| **Both** | Pyroscope | 上方表格+下方火焰图，联动高亮 |
| **Sandwich** | speedscope | 选中函数后显示 callers + callees |
| **Diff** | Pyroscope | 两时间段对比，红增绿减 |

**交互**：

| 操作 | 效果 |
|------|------|
| 滚轮缩放 | 水平缩放时间轴 |
| 拖拽平移 | Configuration Space 中移动 |
| 双击帧 | 聚焦到该帧 (zoom to fit) |
| 单击帧 | 显示函数详情 |
| Cmd/Ctrl+F | 搜索并高亮匹配帧 |
| 右键帧 | "Focus on subtree" / "Sandwich view" |

**火焰图渲染规格**（参考 speedscope WebGL 方案）：
- 行高：22px
- 颜色：HSL 色轮，按函数名哈希分配
- 悬停：tooltip 显示函数名 + self% + total% + 样本数
- 文字：仅宽度 > 40px 的帧显示文字（Canvas 2D 层）

### 5.4 录制回放（全局功能，非独立页面）

录制回放作为全局功能嵌入现有 UI，不需要独立页面。所有的实时显示窗口/图表同时也是回放的显示窗口/图表。

**TopBar 录制控件**：

```
[TimeRange] ─── [SSE Status] ─── [⏺ Record ▾] [⬆ Import] ─── [🔴 Live]
                                       │
                                       ▼ 下拉菜单
                               ┌────────────────────┐
                               │ ⏺ Start Recording  │
                               │ ■ Stop Recording   │
                               │ ────────────────── │
                               │ ⬇ Export (.ilm)    │
                               │ ⬆ Import (.ilm)    │
                               └────────────────────┘
```

**录制状态指示**：
- **录制中**：TopBar Record 按钮变红 + 脉冲动画 + 显示已录制时长 `REC 02:35`
- **未录制**：常规灰色按钮

**回放模式激活**：
- 导入 `.ilm` 录制文件后，自动切换到回放模式
- TopBar 出现回放控制条（替代时间范围选择器位置）：

```
[|◀] [▶/⏸] [▶|]  ──●──────────── 02:30 / 05:00  [1x ▾] [✕ 退出回放]
```

- 所有页面（Overview/Feature Detail/Flame Graph）的图表自动切换数据源：Live SSE → 录制回放数据
- StatusBar 显示 `Replay Mode · cpu-spike-2026-07-25.ilm` 替代实时消息统计
- 退出回放后恢复 Live 模式

### 5.5 插件管理页

**参考**：Grafana Variables + Netdata 零配置

布局：左侧 Feature 列表 + 右侧 Schema 驱动配置面板

- Feature 列表：`grid-cols-1 lg:grid-cols-[1fr_400px]`
- 配置面板由 `/api/v2/features/:name/config/schema` 动态生成
- 字段类型映射：`string → input`, `integer → number`, `enum → select`, `boolean → toggle`
- Apply 实时生效，调用 `/api/v2/features/:name/reconfigure`
- 支持 "Load Plugin (.so)" 按钮上传新插件

---

## 六、图表与可视化规格

参考 `ui-ux-pro-max --domain chart` 的推荐：

### 6.1 时序图（Streaming Area Chart）

| 属性 | 规格 |
|------|------|
| 库 | ECharts (tree-shaking: LineChart + GridComponent + TooltipComponent + DataZoomComponent) |
| 高度 | 320-360px |
| 更新频率 | ≤1Hz 正常渲染，≥1Hz Canvas 缓冲 |
| 降采样 | >2000 点使用 LTTB 算法 |
| 阈值线 | Warning (amber 虚线) + Critical (red 虚线) |
| Tooltip | 显示时间戳 + 各 series 值 + 分布直方图 (参考 Netdata) |
| DataZoom | 底部 slider 类型，高 40px |
| 颜色 | 各 series 使用语义色：user=#6366f1, system=#f97316, iowait=#ef4444, idle=#22c55e |

### 6.2 火焰图（WebGL）

| 属性 | 规格 |
|------|------|
| 渲染 | 双 Canvas：WebGL (矩形) + Canvas 2D (文字) |
| 行高 | 22px |
| 文字阈值 | 帧宽 > 40px 时显示 |
| 颜色 | HSL 色轮，hue = hash(function_name) % 360 |
| 性能 | 60fps，支持 100MB+ profile |
| 缩放 | 滚轮水平缩放，最大 100x |
| 平移 | 鼠标拖拽 |

### 6.3 热力图（Heatmap）

| 属性 | 规格 |
|------|------|
| 用途 | Per-Core CPU / 延迟分布 |
| 库 | ECharts HeatmapChart |
| 颜色渐变 | Cool (#1e40af) → Warm (#ef4444) |
| 格子大小 | 自适应 |
| Tooltip | 行标签 + 列标签 + 值 |

### 6.4 Sparkline（SVG）

| 属性 | 规格 |
|------|------|
| 渲染 | 纯 SVG path，无依赖 |
| 高度 | 32-40px |
| 宽度 | 100% (flex) |
| 线宽 | 1.5px |
| 填充 | 8% 透明度 area fill |
| 端点 | strokeLinecap="round", strokeLinejoin="round" |

---

## 七、数据流架构

### 7.1 SSE 数据管道

```
Backend SSE (/api/v1/sse)
        │ EventSource
        ▼
   SseLink  ← 连接管理 + 自动重连 + 指数退避
        │ raw messages
        ▼
   DataBus  ← 分帧重组 + 按 feature 路由 + 消息统计(msg/s, drops)
        │ typed payloads
    ┌───┼────────┐
    ▼   ▼        ▼
 TimeSeries  Profile  Connection
  Store      Store    Store
    │         │
    ▼         ▼
 ECharts   WebGL
```

### 7.2 Store 设计

| Store | 职责 | 持久范围 |
|-------|------|---------|
| `useConnectionStore` | SSE 状态、延迟、重连计数 | App 全局 |
| `useFeatureStore` | Feature 列表、状态、schema | App 全局 |
| `useTimeStore` | 全局时间窗口 (1m/5m/15m/30m/1h) | App 全局 |
| `useTimeSeriesStore` | Metrics 环形缓冲 (3600 点 = 1h@1Hz) | 页面切换保留 |
| `useProfileStore` | Profile 采样快照、帧聚合 | 切换 Feature 清空 |
| `useRecordingStore` | 录制状态、录制列表、回放进度 | App 全局 |
| `useUIStore` | 侧边栏折叠、主题、面板状态 | App 全局 + localStorage |

### 7.3 录制回放数据流（全局模式切换）

录制和回放通过 `DataBus` 层透明切换数据源，所有下游 Store/UI 无需感知：

```
                 ┌─ Live:   SSE → SseLink ─┐
DataBus 输入  ← ┤                          ├→ DataBus → Stores → UI
                 └─ Replay: File → Replay  ─┘
                             Engine (speed: 0.5x-4x, scrubber)

录制时：DataBus → (fork) → RecordingBuffer → .ilm file (export)
```

- **切换机制**：`useRecordingStore` 中的 `mode: 'live' | 'replay'` 控制 DataBus 输入源
- **透明性**：下游 TimeSeriesStore / ProfileStore 等无需区分数据来源
- **录制格式**：`.ilm` = gzip(ndjson)，每行一个 SSE message + timestamp

---

## 八、交互与动画规格

参考 `ui-ux-pro-max` (motion=6, Standard tier)：

### 8.1 入场动画

| 元素 | 动画 | 时长 | 延迟 |
|------|------|------|------|
| 页面标题 | `fade-in-up` (opacity 0→1, Y 16px→0) | 400ms | 0ms |
| 面板/卡片 | `fade-in-up` + stagger | 500ms | `i * 80ms` |
| 侧边栏 Logo | `slide-in-left` | 300ms | 0ms |
| KPI 数值 | CountUp (easeOutQuart) | 1200ms | 卡片入场后 |

### 8.2 状态反馈

| 交互 | 效果 | 时长 |
|------|------|------|
| 按钮 Hover | 背景色变化 + transition | 200ms |
| 卡片 Hover | `card-glow` 阴影 + border 提亮 | 300ms |
| 导航项 Active | 左侧 accent 竖条 + 背景高亮 | 200ms |
| SSE 连接 | 绿色圆点 pulse-glow | 2s ease-in-out infinite |
| Toggle 切换 | 圆形滑块 translateX | 200ms |
| 录制中 | 红色圆点 pulse | 1.5s infinite |

### 8.3 无障碍

| 要求 | 实现 |
|------|------|
| `prefers-reduced-motion` | 所有动画使用 `@media (prefers-reduced-motion: reduce)` 降级为 instant |
| 键盘导航 | 所有按钮/链接可 Tab 聚焦，Enter 触发 |
| Focus visible | 使用 `focus-visible:ring-2 ring-accent` |
| ARIA | 状态变化使用 `aria-live="polite"` 通知屏读器 |
| 对比度 | 主文本 ≥ 4.5:1，辅助文本 ≥ 3:1 |

---

## 九、组件清单

### 9.1 布局组件

| 组件 | 说明 |
|------|------|
| `<Sidebar>` | 可折叠侧边栏，260px / 64px |
| `<TopBar>` | 时间选择 + SSE 状态 + 录制 |
| `<StatusBar>` | 底部状态栏 |
| `<PageContainer>` | 页面包裹层，`page-container` CSS class |

### 9.2 数据展示组件

| 组件 | 说明 |
|------|------|
| `<StatCard>` | 金信号卡片：标签 + CountUp + Sparkline + Trend |
| `<FeatureCard>` | Feature 卡片：状态灯 + 名称 + 指标 + Sparkline + Actions |
| `<SparkLine>` | SVG sparkline 迷你图 |
| `<CountUp>` | 数值入场动画 |
| `<ShinyText>` | 文字 shimmer 特效 |

### 9.3 图表组件

| 组件 | 说明 |
|------|------|
| `<TimeSeriesChart>` | ECharts 时序面积图 |
| `<FlameGraphCanvas>` | WebGL + Canvas 火焰图 |
| `<HeatmapChart>` | ECharts 热力图 |
| `<BulletChart>` | KPI bullet chart (未来) |

### 9.4 交互组件

| 组件 | 说明 |
|------|------|
| `<ControlBar>` | Feature 控制栏 (Pause/Resume/Config/Record/Reset) |
| `<ConfigPanel>` | Schema 驱动配置面板 |
| `<TimelineScrubber>` | 录制回放时间轴 |
| `<SearchBar>` | 火焰图函数搜索 |
| `<ViewModeToggle>` | 火焰图视图模式切换 |

---

## 十、性能优化策略

参考 `ui-ux-pro-max` React stack guidelines + `--domain chart` 建议：

| 策略 | 参考 | 细节 |
|------|------|------|
| **WebGL 火焰图** | speedscope | 双 canvas，60fps 100MB+ |
| **环形缓冲** | Netdata | 固定 3600 点 (1h@1Hz) |
| **LTTB 降采样** | Grafana | >2000 点降采样，保持曲线形态 |
| **Web Worker** | speedscope + Perfetto | 火焰图处理、大数据集转换 |
| **按需 SSE** | Netdata | 仅订阅当前页面 Feature |
| **虚拟列表** | React stack skill | 进程/线程表使用 `@tanstack/react-virtual` |
| **React.lazy** | React stack skill | 路由和重型组件 lazy load |
| **Canvas 缓冲** | chart skill | ≥1Hz 数据使用 Canvas 缓冲最近 60-300s |
| **ECharts tree-shaking** | 实践 | 仅导入 LineChart + Grid + Tooltip + DataZoom |

---

## 十一、开发阶段规划

| 阶段 | 内容 | 依赖 |
|------|------|------|
| **Phase 1: 基础设施** | shadcn/ui init + Tailwind tokens + 布局骨架 + Router | 无 |
| **Phase 2: 数据层** | SseLink + DataBus + Zustand Stores | Phase 1 |
| **Phase 3: Overview** | StatCard + FeatureCard + CountUp + SparkLine | Phase 1 |
| **Phase 4: 时序图** | TimeSeriesChart + HeatmapChart + Feature 详情模板 | Phase 2 |
| **Phase 5: 火焰图** | WebGL FlameGraph + Table + Sandwich + Diff | Phase 2 |
| **Phase 6: 各 Feature 页** | CPU / Memory / I/O / Network / Sched | Phase 4 |
| **Phase 7: 插件管理** | Schema 驱动 ConfigPanel + .so 加载 | Phase 2 |
| **Phase 8: 录制回放** | TopBar 录制控件 + RecordingBuffer + ReplayEngine + 回放控制条 | Phase 2 |
| **Phase 9: 优化** | 虚拟列表 + Web Worker + LTTB + 性能审计 | Phase 4-8 |

---

## 十二、Pre-Delivery Checklist

来自 `ui-ux-pro-max` skill：

- [ ] 无 Emoji 作为图标（统一使用 Lucide React SVG）
- [ ] 所有可点击元素有 `cursor-pointer`
- [ ] Hover 状态有平滑 transition (150-300ms)
- [ ] 文本对比度 ≥ 4.5:1 (dark mode)
- [ ] Focus 状态对键盘导航可见
- [ ] 尊重 `prefers-reduced-motion`
- [ ] 在 375px / 768px / 1024px / 1440px / 1920px 下测试
- [ ] 8dp 间距节奏一致
- [ ] 所有色彩使用 CSS Token（无硬编码 hex）
- [ ] 所有图表有 Pause/Resume 控制（实时流场景）
