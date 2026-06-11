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
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// ProcMapEntry: 描述 /proc/<pid>/maps 中一条内存映射条目
// 对应字段：起始地址、结束地址、文件内偏移、文件路径
struct ProcMapEntry {
    uint64_t start = 0;     // 虚拟地址空间起始位置
    uint64_t end = 0;       // 虚拟地址空间结束位置
    uint64_t offset = 0;    // 在映射文件内的偏移量
    std::string path;       // 映射的 ELF 文件路径
};

// 解析 /proc/<pid>/maps 中的一行文本
// 格式示例: "7f1234000000-7f1234001000 r-xp 00000000 08:01 123456 /lib/x86_64-linux-gnu/libc.so.6"
// 参数:
//   line - maps 文件的一行内容
//   out  - 输出参数，解析成功时填充 ProcMapEntry
// 返回:
//   true 表示解析成功，false 表示格式不合法
inline bool ParseProcMapsLine(const std::string& line, ProcMapEntry* out) {
    uint64_t start = 0;
    uint64_t end = 0;
    char perms[8] = {};
    uint64_t offset = 0;
    char dev[16] = {};
    uint64_t inode = 0;
    char pathbuf[1024] = {};

    int n = sscanf(line.c_str(), "%lx-%lx %7s %lx %15s %lu %1023[^\n]", &start,
                   &end, perms, &offset, dev, &inode, pathbuf);
    if (n < 6)
        return false;

    out->start = start;
    out->end = end;
    out->offset = offset;
    out->path = (n >= 7) ? std::string(pathbuf) : std::string();
    // 去除路径开头的空白字符
    while (!out->path.empty() &&
           (out->path.front() == ' ' || out->path.front() == '\t'))
        out->path.erase(0, 1);
    return true;
}

// ElfSymbolCache: ELF 文件符号缓存
// 负责加载 ELF 文件的符号表（.symtab 优先，回退到 .dynsym），
// 并按地址排序以支持二分查找。
// 仅缓存 STT_FUNC、STT_GNU_IFUNC 和 STT_OBJECT 类型符号。
class ElfSymbolCache {
public:
    // 从指定路径加载 ELF 文件并解析符号表
    // 参数:
    //   path - ELF 文件的完整路径
    // 返回:
    //   true 表示成功加载并至少有一个有效符号
    bool Load(const std::string& path) {
        symbols_.clear();
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;

        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (buf.size() < sizeof(Elf64_Ehdr))
            return false;

        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buf.data());
        if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
            ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
            ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
            ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
            ehdr->e_ident[EI_CLASS] != ELFCLASS64)
            return false;

        if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr))
            return false;

        // 解析 program headers 获取 ELF load base（第一个 PT_LOAD 段的 p_vaddr）
        // 对 PIE (ET_DYN) base=0，对 non-PIE (ET_EXEC) base 通常为 0x400000
        // 将 sym.st_value 减去 base 以归一化为文件相对偏移
        uint64_t load_base = 0;
        if (ehdr->e_phoff != 0 && ehdr->e_phentsize == sizeof(Elf64_Phdr)) {
            auto* phdrs = reinterpret_cast<Elf64_Phdr*>(buf.data() + ehdr->e_phoff);
            uint64_t min_vaddr = UINT64_MAX;
            for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
                if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < min_vaddr) {
                    min_vaddr = phdrs[i].p_vaddr;
                }
            }
            if (min_vaddr != UINT64_MAX)
                load_base = min_vaddr;
        }

        auto* shdrs =
            reinterpret_cast<Elf64_Shdr*>(buf.data() + ehdr->e_shoff);

        const Elf64_Sym* symtab = nullptr;
        size_t sym_count = 0;
        const char* strtab = nullptr;

        for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
            const Elf64_Shdr& sh = shdrs[i];
            if (sh.sh_type == SHT_SYMTAB && symtab == nullptr) {
                symtab =
                    reinterpret_cast<const Elf64_Sym*>(buf.data() + sh.sh_offset);
                sym_count = sh.sh_size / sizeof(Elf64_Sym);
                const Elf64_Shdr& str_sec = shdrs[sh.sh_link];
                strtab = buf.data() + str_sec.sh_offset;
            }
        }

        if (!symtab || !strtab) {
            for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
                const Elf64_Shdr& sh = shdrs[i];
                if (sh.sh_type == SHT_DYNSYM) {
                    symtab = reinterpret_cast<const Elf64_Sym*>(buf.data() +
                                                                sh.sh_offset);
                    sym_count = sh.sh_size / sizeof(Elf64_Sym);
                    const Elf64_Shdr& str_sec = shdrs[sh.sh_link];
                    strtab = buf.data() + str_sec.sh_offset;
                    break;
                }
            }
        }

        if (!symtab || !strtab)
            return false;

        for (size_t i = 0; i < sym_count; ++i) {
            const Elf64_Sym& sym = symtab[i];
            unsigned char info = ELF64_ST_TYPE(sym.st_info);
            if (sym.st_name == 0)
                continue;
            if (info != STT_FUNC && info != STT_GNU_IFUNC && info != STT_OBJECT)
                continue;
            if (sym.st_value == 0 && sym.st_size == 0)
                continue;
            const char* name = strtab + sym.st_name;
            if (!name || name[0] == '\0')
                continue;
            // 归一化：减去 load_base 将绝对地址转换为文件相对偏移
            uint64_t normalized = sym.st_value >= load_base
                ? sym.st_value - load_base : sym.st_value;
            symbols_.push_back({normalized, std::string(name), sym.st_size});
        }

        std::sort(symbols_.begin(), symbols_.end(),
                  [](const auto& a, const auto& b) {
                      return a.value < b.value;
                  });
        return !symbols_.empty();
    }

    // 根据文件内地址查找符号名称
    // 参数:
    //   addr_in_file - 相对于 ELF 文件基址的偏移地址
    // 返回:
    //   符号名，若未找到则返回空字符串
    std::string Resolve(uint64_t addr_in_file) const {
        if (symbols_.empty())
            return {};

        // 二分查找：定位到最后一个 value <= addr_in_file 的符号
        auto it = std::upper_bound(
            symbols_.begin(), symbols_.end(), addr_in_file,
            [](uint64_t val, const Sym& s) { return val < s.value; });
        if (it == symbols_.begin())
            return {};
        --it;
        // 若有 size，校验地址是否在符号范围内
        if (it->size != 0 && addr_in_file >= it->value + it->size)
            return {};
        return it->name;
    }

private:
    // 内部符号表示：地址、名称、大小
    struct Sym {
        uint64_t value = 0;
        std::string name;
        uint64_t size = 0;
    };
    std::vector<Sym> symbols_;
};

// KernelSymbolResolver: 内核符号解析器
// 通过读取 /proc/kallsyms 获取内核函数和代码段符号。
// 仅保留 t/T/w/W 类型（文本段/弱符号）以确保只解析代码地址。
class KernelSymbolResolver {
public:
    // 从 /proc/kallsyms 加载内核符号表
    // 返回:
    //   Status::Ok() 表示成功加载
    Status Load() {
        addrs_.clear();
        std::ifstream f("/proc/kallsyms");
        if (!f)
            return Status::Error(StatusCode::kInternal,
                                 "cannot open /proc/kallsyms");

        std::string line;
        while (std::getline(f, line)) {
            uint64_t addr = 0;
            char t = 0;
            char name[512] = {};
            if (std::sscanf(line.c_str(), "%" SCNx64 " %c %511s", &addr, &t,
                            name) != 3)
                continue;
            // 只取代码段符号 (t/T) 和弱符号 (w/W)
            if (t != 't' && t != 'T' && t != 'w' && t != 'W')
                continue;
            addrs_.push_back({addr, std::string(name)});
        }

        // 按地址排序，支持二分查找
        std::sort(addrs_.begin(), addrs_.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        return Status::Ok();
    }

    // 根据内核地址查找符号名
    // 参数:
    //   addr - 内核地址空间中的绝对地址
    // 返回:
    //   符号名，未找到返回空字符串
    std::string Resolve(uint64_t addr) const {
        if (addrs_.empty())
            return {};

        auto it = std::upper_bound(
            addrs_.begin(), addrs_.end(), addr,
            [](uint64_t a, const std::pair<uint64_t, std::string>& b) {
                return a < b.first;
            });
        if (it == addrs_.begin())
            return {};
        --it;
        return it->second;
    }

private:
    // (地址, 符号名) 的有序集合
    std::vector<std::pair<uint64_t, std::string>> addrs_;
};

// MapsCacheEntry: /proc/<pid>/maps 文件的缓存条目
// 包含解析后的映射列表及其加载时间戳，用于 TTL 过期控制
struct MapsCacheEntry {
    std::vector<ProcMapEntry> maps;
    std::chrono::steady_clock::time_point loaded_at{};
};

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
    // 对 C++ 修饰名执行 demangle，转换为可读的函数签名
    // 使用 __cxa_demangle 进行转换
    // 参数:
    //   sym - 可能被修饰的符号名称
    // 返回:
    //   反修饰后的可读名称，失败则返回原字符串
    std::string DemangleMaybe(const std::string& sym) const {
        if (!demangle_ || sym.empty())
            return sym;
        int status = 0;
        char* dm =
            abi::__cxa_demangle(sym.c_str(), nullptr, nullptr, &status);
        if (status != 0 || !dm)
            return sym;
        std::string out(dm);
        std::free(dm);
        return out;
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
        if (!f)
            return false;

        std::string line;
        while (std::getline(f, line)) {
            ProcMapEntry m{};
            if (!ParseProcMapsLine(line, &m))
                continue;
            // 只缓存以 '/' 开头的绝对路径映射（可执行文件和共享库）
            if (m.path.empty() || m.path[0] != '/')
                continue;
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
        if (!cache.Load(path))
            return nullptr;

        auto ins = elf_cache_.emplace(path, std::move(cache));
        return &ins.first->second;
    }

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

            ElfSymbolCache* elf = GetElfCache(m.path);
            if (!elf)
                break;

            uint64_t off = addr - m.start + m.offset;
            std::string sym = elf->Resolve(off);
            if (!sym.empty())
                return sym;

            // 符号解析失败，使用 "<path>+<offset>" 格式作为回退
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
