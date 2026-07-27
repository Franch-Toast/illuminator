// ============================================================================
// Stack symbol resolution utilities (shared by Source and Processor layers)
// ============================================================================

#pragma once

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/common/status.h"

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

        // 收集所有符号表段（.symtab 和 .dynsym 合并，maximizing coverage）
        struct SymTabInfo {
            const Elf64_Sym* syms;
            size_t count;
            const char* strtab;
        };
        std::vector<SymTabInfo> sym_tables;

        for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
            const Elf64_Shdr& sh = shdrs[i];
            if (sh.sh_type != SHT_SYMTAB && sh.sh_type != SHT_DYNSYM)
                continue;
            if (sh.sh_offset + sh.sh_size > buf.size())
                continue;
            if (sh.sh_link >= ehdr->e_shnum)
                continue;
            const Elf64_Shdr& str_sec = shdrs[sh.sh_link];
            if (str_sec.sh_offset + str_sec.sh_size > buf.size())
                continue;

            SymTabInfo info{};
            info.syms = reinterpret_cast<const Elf64_Sym*>(
                buf.data() + sh.sh_offset);
            info.count = sh.sh_size / sizeof(Elf64_Sym);
            info.strtab = buf.data() + str_sec.sh_offset;
            sym_tables.push_back(info);
        }

        if (sym_tables.empty())
            return false;

        for (const auto& tab : sym_tables) {
            for (size_t i = 0; i < tab.count; ++i) {
                const Elf64_Sym& sym = tab.syms[i];
                unsigned char info = ELF64_ST_TYPE(sym.st_info);
                if (sym.st_name == 0)
                    continue;
                if (info != STT_FUNC && info != STT_GNU_IFUNC && info != STT_OBJECT)
                    continue;
                if (sym.st_value == 0 && sym.st_size == 0)
                    continue;
                const char* name = tab.strtab + sym.st_name;
                if (!name || name[0] == '\0')
                    continue;
                uint64_t normalized = sym.st_value >= load_base
                    ? sym.st_value - load_base : sym.st_value;
                symbols_.push_back({normalized, std::string(name), sym.st_size});
            }
        }

        std::sort(symbols_.begin(), symbols_.end(),
                  [](const auto& a, const auto& b) {
                      if (a.value != b.value) return a.value < b.value;
                      // 优先保留有 size 信息的符号
                      return a.size > b.size;
                  });
        // 去重：相同地址保留第一个（有 size 的优先）
        symbols_.erase(
            std::unique(symbols_.begin(), symbols_.end(),
                        [](const auto& a, const auto& b) {
                            return a.value == b.value && a.name == b.name;
                        }),
            symbols_.end());
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

        auto it = std::upper_bound(
            symbols_.begin(), symbols_.end(), addr_in_file,
            [](uint64_t val, const Sym& s) { return val < s.value; });
        if (it == symbols_.begin())
            return {};
        --it;

        if (it->size != 0) {
            if (addr_in_file < it->value + it->size)
                return it->name;
            // 地址在符号 size 之外，尝试 nearest-symbol 启发式
            return ResolveNearest(it, addr_in_file);
        }
        // size=0：使用到下一个符号的距离作为隐式边界
        auto next = it + 1;
        if (next != symbols_.end()) {
            uint64_t gap = next->value - it->value;
            if (addr_in_file < it->value + gap)
                return it->name;
        } else {
            // 最后一个符号且 size=0：允许合理的偏移范围（4KB）
            if (addr_in_file - it->value < 4096)
                return it->name;
        }
        return {};
    }

private:
    struct Sym {
        uint64_t value = 0;
        std::string name;
        uint64_t size = 0;
    };
    std::vector<Sym> symbols_;

    std::string ResolveNearest(
        std::vector<Sym>::const_iterator lower_it,
        uint64_t addr) const {
        uint64_t dist = addr - (lower_it->value + lower_it->size);
        if (dist > 512)
            return {};
        auto upper_it = lower_it + 1;
        if (upper_it != symbols_.end() && addr < upper_it->value) {
            return lower_it->name + "+gap";
        }
        return {};
    }
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

// ============================================================================
// 共享符号解析工具函数
// ============================================================================

// C++ 名称反修饰（demangle）
inline std::string DemangleSymbol(const std::string& sym, bool enabled = true) {
    if (!enabled || sym.empty()) return sym;
    int status = 0;
    char* dm = abi::__cxa_demangle(sym.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !dm) return sym;
    std::string out(dm);
    std::free(dm);
    return out;
}

// 从 ELF 文件中提取 .note.gnu.build-id 的十六进制字符串
inline std::string ExtractBuildId(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};

    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (buf.size() < sizeof(Elf64_Ehdr)) return {};

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buf.data());
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0) return {};
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr))
        return {};

    auto* shdrs = reinterpret_cast<Elf64_Shdr*>(buf.data() + ehdr->e_shoff);
    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        if (shdrs[i].sh_type != SHT_NOTE) continue;
        if (shdrs[i].sh_offset + shdrs[i].sh_size > buf.size()) continue;

        const char* note_data = buf.data() + shdrs[i].sh_offset;
        size_t remaining = shdrs[i].sh_size;
        size_t pos = 0;
        while (pos + 12 <= remaining) {
            uint32_t namesz = *reinterpret_cast<const uint32_t*>(note_data + pos);
            uint32_t descsz = *reinterpret_cast<const uint32_t*>(note_data + pos + 4);
            uint32_t type = *reinterpret_cast<const uint32_t*>(note_data + pos + 8);
            size_t name_start = pos + 12;
            size_t name_aligned = (namesz + 3) & ~3u;
            size_t desc_start = name_start + name_aligned;
            size_t desc_aligned = (descsz + 3) & ~3u;

            if (desc_start + descsz > remaining) break;

            if (type == 3 && namesz == 4 &&
                std::memcmp(note_data + name_start, "GNU", 4) == 0) {
                std::string hex;
                hex.reserve(descsz * 2);
                for (size_t j = 0; j < descsz; ++j) {
                    char h[3];
                    std::snprintf(h, sizeof(h), "%02x",
                                  static_cast<uint8_t>(note_data[desc_start + j]));
                    hex += h;
                }
                return hex;
            }
            pos = desc_start + desc_aligned;
        }
    }
    return {};
}

// 尝试通过 build-id 或标准调试路径加载 ELF 调试符号
inline bool TryLoadDebugInfo(const std::string& elf_path, ElfSymbolCache& cache) {
    std::string build_id = ExtractBuildId(elf_path);
    if (build_id.size() >= 4) {
        std::string bid_path = "/usr/lib/debug/.build-id/"
            + build_id.substr(0, 2) + "/" + build_id.substr(2) + ".debug";
        if (cache.Load(bid_path)) return true;
    }

    std::string debug_path = "/usr/lib/debug" + elf_path + ".debug";
    if (cache.Load(debug_path)) return true;
    debug_path = "/usr/lib/debug" + elf_path;
    if (cache.Load(debug_path)) return true;

    auto last_slash = elf_path.rfind('/');
    if (last_slash != std::string::npos) {
        std::string dir = elf_path.substr(0, last_slash + 1);
        std::string base = elf_path.substr(last_slash + 1);
        debug_path = dir + ".debug/" + base + ".debug";
        if (cache.Load(debug_path)) return true;
        debug_path = dir + ".debug/" + base;
        if (cache.Load(debug_path)) return true;
    }
    return false;
}

}  // namespace illuminator
