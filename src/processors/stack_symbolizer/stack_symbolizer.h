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
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

struct ProcMapEntry {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t offset = 0;
    std::string path;
};

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
    while (!out->path.empty() &&
           (out->path.front() == ' ' || out->path.front() == '\t'))
        out->path.erase(0, 1);
    return true;
}

class ElfSymbolCache {
public:
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
            // Fall back to dynamic symbol table
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
            symbols_.push_back({sym.st_value, std::string(name), sym.st_size});
        }

        std::sort(symbols_.begin(), symbols_.end(),
                  [](const auto& a, const auto& b) {
                      return a.value < b.value;
                  });
        return !symbols_.empty();
    }

    std::string Resolve(uint64_t addr_in_file) const {
        if (symbols_.empty())
            return {};

        auto it = std::upper_bound(
            symbols_.begin(), symbols_.end(), addr_in_file,
            [](uint64_t val, const Sym& s) { return val < s.value; });
        if (it == symbols_.begin())
            return {};
        --it;
        if (it->size != 0 && addr_in_file >= it->value + it->size)
            return {};
        return it->name;
    }

private:
    struct Sym {
        uint64_t value = 0;
        std::string name;
        uint64_t size = 0;
    };
    std::vector<Sym> symbols_;
};

class KernelSymbolResolver {
public:
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
            if (t != 't' && t != 'T' && t != 'w' && t != 'W')
                continue;
            addrs_.push_back({addr, std::string(name)});
        }

        std::sort(addrs_.begin(), addrs_.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        return Status::Ok();
    }

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
    std::vector<std::pair<uint64_t, std::string>> addrs_;
};

struct MapsCacheEntry {
    std::vector<ProcMapEntry> maps;
    std::chrono::steady_clock::time_point loaded_at{};
};

class StackSymbolizerProcessor : public ProcessorPlugin {
public:
    const char* Name() const override { return "stack_symbolizer"; }
    const char* Version() const override { return "0.1.0"; }

    Status Init(const ConfigValue& config) override {
        demangle_ = config["demangle"].AsBool(true);
        kernel_symbols_ = config["kernel_symbols"].AsBool(true);
        cache_ttl_sec_ =
            static_cast<uint32_t>(config["cache_ttl_sec"].AsInt(30));
        jit_map_ = config["jit_map"].AsBool(false);
        (void)jit_map_;

        if (kernel_symbols_) {
            auto st = kernel_resolver_.Load();
            if (!st.ok())
                IL_WARN("stack_symbolizer: kernel symbols unavailable: %s",
                        st.message().c_str());
        }
        return Status::Ok();
    }

    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        if (!input)
            return input;

        const auto now = std::chrono::steady_clock::now();
        PurgeExpiredMapsCaches(now);

        for (auto& sample : input->stack_samples()) {
            for (auto& fr : sample.kernel_stack) {
                if (fr.address == 0)
                    continue;
                std::string sym =
                    kernel_symbols_ ? kernel_resolver_.Resolve(fr.address)
                                      : std::string();
                if (sym.empty()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[kernel 0x%llx]",
                                  static_cast<unsigned long long>(fr.address));
                    sym = buf;
                }
                sym = DemangleMaybe(sym);
                fr.function_name = input->InternString(sym);
            }

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
            if (m.path.empty() || m.path[0] != '/')
                continue;
            entry->maps.push_back(std::move(m));
        }
        entry->loaded_at = std::chrono::steady_clock::now();
        return true;
    }

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

    std::string ResolveUser(uint32_t pid, uint64_t addr,
                            std::chrono::steady_clock::time_point now) {
        MapsCacheEntry* ment = nullptr;
        auto it = maps_cache_.find(pid);
        if (it == maps_cache_.end()) {
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
            if (cache_ttl_sec_ > 0 && age > cache_ttl_sec_) {
                LoadMaps(pid, ment);
            }
        }

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

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                          m.path.c_str(),
                          static_cast<unsigned long long>(addr - m.start));
            return std::string(buf);
        }
        return {};
    }

    bool demangle_ = true;
    bool kernel_symbols_ = true;
    uint32_t cache_ttl_sec_ = 30;
    bool jit_map_ = false;

    KernelSymbolResolver kernel_resolver_;
    std::unordered_map<std::string, ElfSymbolCache> elf_cache_;
    std::unordered_map<uint32_t, MapsCacheEntry> maps_cache_;
};

IL_REGISTER_PROCESSOR("stack_symbolizer", StackSymbolizerProcessor);

}  // namespace illuminator
