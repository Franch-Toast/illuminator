# Illuminator Core

Illuminator 当前提供 C++20 Pipeline v3 引擎、插件系统、数据模型和配置系统，以及独立的录制 Sink、SQLite 存储和最小 CLI。应用编排与界面层已移除，后续可基于这些核心能力重新设计。

```text
TimerWheel → CollectPool → Source::Collect()
                                ↓
                          AsyncChannel
                                ↓
                         ProcessThread
                   Processor[] → Aggregator?
                                ↓
                            SinkPool
                                ↓
                             Sink[]
```

## 构建与验证

需要 Linux、C++20 编译器、Bazel 7.6.1（或 Bazelisk）、Python 3，以及系统开发库 `libbpf-dev libelf-dev zlib1g-dev libsqlite3-dev`。Bazel 自动获取 spdlog、nlohmann/json、yaml-cpp 和 GoogleTest。Clang/bpftool 仅用于后续 eBPF 探针开发。

```bash
make check-env
make build
make test

# 无需 root，运行 1 秒合成数据 Source → RecordingSink
make demo
# 或指定目录（支持空格）
./bazel-bin/src/cli/illuminator demo --duration 2 --output-dir /tmp/illuminator-demo
```

CLI 输出处理批数、记录数、错误数和 `.ilr` 文件路径；退出前停止定时调度，等待采集完成、排空处理通道和异步写入，再关闭录制文件。SIGINT/SIGTERM 使用同一停机路径。

## 目录

| 路径 | 职责 |
|------|------|
| `src/core/common` | Status、ConfigValue、GlobalConfig、YAML、DataBatch、JSON、日志 |
| `src/core/engine` | Pipeline、AsyncChannel、TimerWheel、InfrastructureManager、PidManager、自观测 |
| `src/core/memory` / `threading` | Arena、队列、线程池 |
| `src/plugin/api` | Source / Processor / Aggregator / Sink 接口与 eBPF Source 基类 |
| `src/plugin/infra` | PluginRegistry、PluginManager、SoLoader、内建插件注册 |
| `src/plugin/sinks/recording_sink` | 独立 NDJSON 录制输出 |
| `src/storage` | StorageBackend、StorageFactory、SQLite 实现与测试 |
| `src/ebpf_common` | 内核能力探测、BPF 工具和构建规则 |
| `src/cli` | 最小运行宿主及冒烟测试；合成 Source 仅用于 demo |

## CLI

```bash
./bazel-bin/src/cli/illuminator version
./bazel-bin/src/cli/illuminator plugins
./bazel-bin/src/cli/illuminator storage
./bazel-bin/src/cli/illuminator --help

# 从 global.plugin_dirs 加载 .so，按名称创建 Source 和 Sink
./bazel-bin/src/cli/illuminator run --source my_source --sink recording \
  --config illuminator.yaml --duration 5 --output-dir /tmp/recordings
```

`demo` 的计数器是合成数据，不代表系统 CPU 指标。`run` 当前以空 `ConfigValue` 初始化 Source；更复杂的插件配置、Processor/Aggregator 组装由嵌入方通过 SDK 完成。CLI 不提供应用控制服务。

## 配置与持久化

[illuminator.yaml.example](illuminator.yaml.example) 保留全局日志、数据目录、插件目录、线程池和通道参数。CLI 的 `--config` 显式加载文件；未指定时使用 `GlobalConfig` 默认值。旧的服务端与自动启停字段已删除，迁移时应使用新的配置模板。

录制插件是独立 Sink，可直接加入 Pipeline。`.ilr` v1 的 `feature` 元数据字段及 `feature_name` 配置名继续表示流名称，以兼容原有文件格式。SQLite 已迁到 `src/storage`，可直接使用 `SqliteBackend` 或 `StorageFactory`；它尚未通过通用 StorageSink 自动接入 Pipeline。

动态插件当前是带 `extern "C"` 描述符入口的 **C++ SDK**：`create()` 需要返回对应的 C++ 插件对象，宿主和插件应使用兼容的工具链。当前不承诺纯 C ABI、跨编译器兼容或运行时安全热卸载。

## 其他命令

```bash
make build-opt
make asan
make tsan
make docker
# 默认执行一次核心 demo，录制目录可挂载持久化
# docker run --rm -v illuminator-data:/var/lib/illuminator illuminator:<tag>
```

CI 保留 C++ 全量构建、核心/插件/存储/CLI 测试、ASAN/TSAN 和核心 CLI 镜像构建。CLI 冒烟测试涵盖真实 `.so` 加载、录制解析、配置错误、路径错误和信号退出。

## 文档

设计文档统一维护在相邻的 Scriptorium 仓库，本仓库保留构建、运行说明和配置样例。完整方案采用总览与专题结构：

- [设计总览与文档导航](../Scriptorium/docs/document/撰修司/illuminator性能监控平台/设计方案/index.md)
- [Pipeline v3 详细设计](../Scriptorium/docs/document/撰修司/illuminator性能监控平台/设计方案/02-pipeline-v3.md)
- [插件系统与扩展契约](../Scriptorium/docs/document/撰修司/illuminator性能监控平台/设计方案/03-plugin-system.md)
- [开发、构建与验证](../Scriptorium/docs/document/撰修司/illuminator性能监控平台/设计方案/09-development-and-validation.md)
- [核心架构审阅与演进](../Scriptorium/docs/document/撰修司/illuminator性能监控平台/设计方案/10-core-audit-and-evolution.md)

上述为工作区内的跨仓库链接，要求两个仓库保持 `illuminator/`、`Scriptorium/` 相邻布局。数据模型、配置、调度、eBPF 和存储等专题可从设计总览进入。

MIT License，见 [LICENSE](LICENSE)。
