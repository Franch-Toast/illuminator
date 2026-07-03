# WASM 插件沙箱运行时设计

> **状态**: 占位设计（Stub 实现）  
> **优先级**: P2（Phase 3）  
> **目标**: 为 Illuminator 提供安全的第三方插件执行环境

---

## 一、动机

当前 Illuminator 插件通过两种方式加载：

1. **内置编译** — C++ 插件通过 `IL_REGISTER_*` 宏静态注册
2. **动态 .so** — 通过 `SoLoader` 加载外部共享库（C ABI）

两者都运行在宿主进程地址空间，无隔离。恶意或有 bug 的插件可能导致：
- 段错误导致整个进程崩溃
- 无限循环阻塞 SinkPool/CollectPool
- 越界读写敏感数据

WASM 沙箱提供：
- **内存隔离** — 插件只能访问自己的线性内存
- **计算限制** — fuel/gas 机制防止无限循环
- **确定性执行** — 相同输入产生相同输出
- **跨平台** — 单一 `.wasm` 二进制运行在 Linux/QNX/macOS

## 二、架构设计

```
┌──────────────────────────────────────────────┐
│             Illuminator Host Process          │
│                                               │
│  ┌─────────────┐     ┌────────────────────┐ │
│  │ Native C++  │     │   WASM Sandbox     │ │
│  │  Plugins    │     │                    │ │
│  │             │     │  ┌──────────────┐  │ │
│  │ Source      │     │  │ wasm module  │  │ │
│  │ Processor   │     │  │ (linear mem) │  │ │
│  │ Sink        │     │  └──────────────┘  │ │
│  │             │     │       ↕ Host API   │ │
│  └─────────────┘     └────────────────────┘ │
│         │                      │              │
│         └──────────┬───────────┘              │
│                    ▼                          │
│         ┌──────────────────┐                 │
│         │ PluginRegistry   │                 │
│         └──────────────────┘                 │
└──────────────────────────────────────────────┘
```

## 三、当前实现（Stub）

```cpp
// src/plugin/wasm/wasm_runtime.h
class WasmRuntime {
public:
    Status LoadModule(const std::string& wasm_path);
    Status CallFunction(const std::string& name, const DataBatch& input,
                       DataBatch& output);
    void SetFuelLimit(uint64_t fuel);
    void SetMemoryLimit(size_t bytes);
};
```

当前为空实现（所有方法返回 `kUnimplemented`），作为未来集成的接口契约。

## 四、VM 后端选型

| 引擎 | 语言 | 特点 | 适合场景 |
|------|------|------|----------|
| WAMR | C | 极轻量 <100KB, AOT 支持 | 嵌入式/QNX |
| Wasmtime | Rust | 生产级, Cranelift JIT | Linux 高性能 |
| Wasmer | Rust | 多编译器后端 | 通用 |
| wasm3 | C | 解释执行, 最小依赖 | 资源受限 |

**推荐**: WAMR（QNX 兼容 + 极低开销）或 Wasmtime（Linux 性能优先）

## 五、Host API 设计（草案）

WASM 插件通过以下导入函数与宿主通信：

```wat
;; 从宿主读取 DataBatch（序列化为 MessagePack）
(import "illuminator" "read_batch" (func $read_batch (param i32 i32) (result i32)))

;; 向宿主写入处理后的 DataBatch
(import "illuminator" "write_batch" (func $write_batch (param i32 i32) (result i32)))

;; 日志输出
(import "illuminator" "log" (func $log (param i32 i32 i32)))

;; 获取配置值
(import "illuminator" "get_config" (func $get_config (param i32 i32 i32 i32) (result i32)))
```

## 六、安全约束

| 约束 | 机制 | 默认值 |
|------|------|--------|
| 内存上限 | WASM linear memory max | 64 MB |
| 计算上限 | Fuel/gas metering | 10M instructions/call |
| 系统调用 | WASI 子集（仅 clock_time_get） | 最小权限 |
| 网络 | 禁止 | — |
| 文件系统 | 禁止 | — |

## 七、集成路径

```
Phase 3.1 — 选定 VM 引擎，编译集成到 Bazel
Phase 3.2 — 实现 WasmProcessorPlugin（Host API + 序列化）
Phase 3.3 — 插件 SDK（Rust/C/AssemblyScript 模板）
Phase 3.4 — 配置支持（illuminator.yaml 中声明 wasm 插件）
Phase 3.5 — 性能基准测试（vs native ProcessorPlugin）
```

## 八、配置示例（未来）

```yaml
pipelines:
  custom_analysis:
    source:
      type: cpu_utilization
    processors:
      - type: wasm_processor
        config:
          module: /etc/illuminator/plugins/my_filter.wasm
          fuel_limit: 5000000
          memory_limit_mb: 32
    sinks:
      - type: sse_sink
```
