// =============================================================================
// 文件：stack_symbolizer.h
// 模块：Illuminator 处理器 - 堆栈符号化处理器
// 描述：
//   将堆栈采样中的原始内存地址解析为可读的符号名称（函数名）。
//   支持两类符号解析：
//     1. 内核栈：通过读取 /proc/kallsyms 获取内核符号表
//     2. 用户栈：通过解析 /proc/<pid>/maps 定位 ELF 文件，再解析 .symtab 或 .dynsym
//   可选支持 C++ 名称反修饰（demangle）。
//   使用带 TTL 的缓存机制避免频繁解析 maps 和 ELF 文件。
// =============================================================================

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <cinttypes>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin/api/processor_plugin.h"
#include "plugin/common/stack_symbol_resolver.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

// StackSymbolizerProcessor: 堆栈符号化处理器
// 主要职责：将堆栈帧中的原始十六进制地址替换为人类可读的函数名。
// 配置参数:
//   demangle       - 是否对 C++ 符号名称执行反修饰，默认 true
//   kernel_symbols - 是否加载内核符号表，默认 true
//   cache_ttl_sec  - maps 缓存过期时间（秒），默认 30
//   jit_map        - 是否尝试解析 JIT 映射（当前预留，未实现）
class StackSymbolizerProcessor : public ProcessorPlugin {
public:
    // 返回处理器名称
    const char* Name() const override { return "stack_symbolizer"; }

    // 返回处理器版本号
    const char* Version() const override { return "0.1.0"; }

    // 初始化配置并预加载内核符号
    // 参数:
    //   config - 配置项，读取 demangle / kernel_symbols / cache_ttl_sec
    // 返回:
    //   Status::Ok() 始终返回成功（内核符号失败仅打印警告）
    Status Init(const ConfigValue& config) override {
        demangle_ = config["demangle"].AsBool(true);
        kernel_symbols_ = config["kernel_symbols"].AsBool(true);
        cache_ttl_sec_ =
            static_cast<uint32_t>(config["cache_ttl_sec"].AsInt(30));
        jit_map_ = config["jit_map"].AsBool(false);
        (void)jit_map_;

        // 若开启内核符号解析，预加载 /proc/kallsyms
        if (kernel_symbols_) {
            auto st = kernel_resolver_.Load();
            if (!st.ok())
                IL_WARN("stack_symbolizer: kernel symbols unavailable: {}",
                        st.message());
        }
        return Status::Ok();
    }

    // 对 DataBatch 中的堆栈帧执行符号化
    // 参数:
    //   input - 包含堆栈采样数据的 DataBatch
    // 返回:
    //   StatusOr<DataBatchPtr> - 符号化后的 DataBatch（就地修改）
    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        if (!input)
            return input;

        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        // 在每次处理前清理过期缓存
        PurgeExpiredMapsCaches(now);

        for (auto& sample : input->stack_samples()) {
            // 解析内核调用栈
            for (auto& fr : sample.kernel_stack) {
                if (fr.address == 0)
                    continue;
                std::string sym =
                    kernel_symbols_ ? kernel_resolver_.Resolve(fr.address)
                                      : std::string();
                // 若未解析到符号，使用地址作为回退标识
                if (sym.empty()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[kernel 0x%llx]",
                                  static_cast<unsigned long long>(fr.address));
                    sym = buf;
                }
                sym = DemangleMaybe(sym);
                fr.function_name = input->InternString(sym);
            }

            // 解析用户态调用栈
            for (auto& fr : sample.user_stack) {
                if (fr.address == 0)
                    continue;
                std::string sym = ResolveUser(sample.pid, fr.address, now);
                if (sym.empty()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[0x%llx]",
                                  static_cast<unsigned long long>(fr.address));
                    sym = buf;
                }
                sym = DemangleMaybe(sym);
                fr.function_name = input->InternString(sym);
            }
        }

        return input;
    }

private:
    std::string DemangleMaybe(const std::string& sym) const {
        return DemangleSymbol(sym, demangle_);
    }

    // 清理已过期的 maps 缓存条目
    // 根据 cache_ttl_sec_ 和条目加载时间判断是否过期
    // 参数:
    //   now - 当前时间点
    void PurgeExpiredMapsCaches(
        std::chrono::steady_clock::time_point now) {
        if (cache_ttl_sec_ == 0)
            return;
        for (auto it = maps_cache_.begin(); it != maps_cache_.end();) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - it->second.loaded_at)
                           .count();
            if (age > cache_ttl_sec_)
                it = maps_cache_.erase(it);
            else
                ++it;
        }
    }

    // 加载指定进程的 /proc/<pid>/maps 文件
    // 参数:
    //   pid   - 目标进程 ID
    //   entry - 输出参数，填充解析后的映射条目和加载时间
    // 返回:
    //   true 表示成功读取并解析
    bool LoadMaps(uint32_t pid, MapsCacheEntry* entry) {
        entry->maps.clear();
        std::string path = "/proc/" + std::to_string(pid) + "/maps";
        std::ifstream f(path);
        if (!f) {
            f.open("/proc/self/maps");
            if (!f)
                return false;
        }

        std::string line;
        while (std::getline(f, line)) {
            ProcMapEntry m{};
            if (!ParseProcMapsLine(line, &m))
                continue;
            if (m.path.empty()) continue;
            if (m.path[0] != '/' && m.path[0] != '[') continue;
            entry->maps.push_back(std::move(m));
        }
        entry->loaded_at = std::chrono::steady_clock::now();
        return true;
    }

    // 获取或创建指定路径的 ELF 符号缓存
    // 参数:
    //   path - ELF 文件路径
    // 返回:
    //   指向 ElfSymbolCache 的指针，若加载失败返回 nullptr
    ElfSymbolCache* GetElfCache(const std::string& path) {
        auto it = elf_cache_.find(path);
        if (it != elf_cache_.end())
            return &it->second;

        ElfSymbolCache cache;
        if (!cache.Load(path)) {
            // 尝试从调试信息路径加载
            if (!TryLoadDebugInfo(path, cache))
                return nullptr;
        }

        auto ins = elf_cache_.emplace(path, std::move(cache));
        return &ins.first->second;
    }

    // 委托给 stack_symbol_resolver.h 中的共享实现

    // 解析用户态地址对应的符号名
    // 流程:
    //   1. 查找或加载目标的 maps 缓存
    //   2. 遍历 maps 条目，找到包含该地址的映射区域
    //   3. 计算文件内偏移 (address - region.start + region.offset)
    //   4. 在对应 ELF 文件中解析符号
    // 参数:
    //   pid  - 目标进程 ID
    //   addr - 用户态虚拟地址
    //   now  - 当前时间（用于缓存过期判断）
    // 返回:
    //   函数名，若无法解析则返回形如 "[<path>+0x<offset>]" 的描述字符串
    std::string ResolveUser(uint32_t pid, uint64_t addr,
                            std::chrono::steady_clock::time_point now) {
        MapsCacheEntry* ment = nullptr;
        auto it = maps_cache_.find(pid);
        if (it == maps_cache_.end()) {
            // 缓存未命中：加载 maps 文件
            MapsCacheEntry fresh{};
            if (!LoadMaps(pid, &fresh))
                return {};
            auto ins = maps_cache_.emplace(pid, std::move(fresh));
            ment = &ins.first->second;
        } else {
            ment = &it->second;
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - ment->loaded_at)
                           .count();
            // 缓存过期则重新加载
            if (cache_ttl_sec_ > 0 && age > cache_ttl_sec_) {
                LoadMaps(pid, ment);
            }
        }

        // 在 maps 条目中查找包含该地址的区域
        for (const auto& m : ment->maps) {
            if (addr < m.start || addr >= m.end)
                continue;

            // 特殊映射快速标注
            if (!m.path.empty() && m.path[0] == '[') {
                if (m.path == "[vdso]") return "__vdso_clock_gettime";
                if (m.path == "[vsyscall]") return "[vsyscall]";
                return m.path;
            }

            ElfSymbolCache* elf = GetElfCache(m.path);
            if (!elf)
                break;

            uint64_t off = addr - m.start + m.offset;
            std::string sym = elf->Resolve(off);
            if (!sym.empty()) {
                // 清理 "+gap" 标记（nearest-symbol 启发式产生）
                if (sym.size() > 4 && sym.substr(sym.size() - 4) == "+gap") {
                    sym = sym.substr(0, sym.size() - 4) + " [inlined]";
                }
                return sym;
            }

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                          m.path.c_str(),
                          static_cast<unsigned long long>(addr - m.start));
            return std::string(buf);
        }
        return {};
    }

    bool demangle_ = true;          // 是否对 C++ 符号执行反修饰
    bool kernel_symbols_ = true;    // 是否解析内核符号
    uint32_t cache_ttl_sec_ = 30;   // maps 缓存过期时间（秒）
    bool jit_map_ = false;          // JIT 映射开关（预留）

    mutable std::mutex mu_;  // guards maps_cache_, elf_cache_, kernel_resolver_ in Process()

    KernelSymbolResolver kernel_resolver_;                           // 内核符号解析器实例
    std::unordered_map<std::string, ElfSymbolCache> elf_cache_;      // ELF 文件符号缓存（按路径索引）
    std::unordered_map<uint32_t, MapsCacheEntry> maps_cache_;        // 进程 maps 缓存（按 PID 索引）
};

// 在插件注册表中注册该处理器
IL_REGISTER_PROCESSOR("stack_symbolizer", StackSymbolizerProcessor);

}  // namespace illuminator
