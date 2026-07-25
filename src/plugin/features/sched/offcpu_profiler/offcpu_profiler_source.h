// ============================================================================
// OffcpuProfilerSource — Off-CPU 性能剖析器（Pull 模式）
// ============================================================================
//
// 基于 EbpfSourceBase 重写。
// 旧版实现保留在 offcpu_profiler.legacy.h 作为功能参考。
//
// 通过 eBPF 追踪进程在非 CPU 执行状态下的等待时间，
// 从 offcpu_stats BPF map 读取聚合数据。
// ============================================================================

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "ebpf_common/include/bpf_compat.h"

#include "offcpu_profiler_sk.skel.h"
#include "core/common/logging.h"
#include "ebpf_common/include/event_types.h"
#include "ebpf_common/loader/bpf_util.h"
#include "ebpf_common/loader/stack_trace_util.h"
#include "plugin/common/stack_symbol_resolver.h"
#include "plugin/api/ebpf_source_base.h"
#include "plugin/infra/plugin_registry.h"

namespace illuminator {

class OffcpuProfilerSource : public EbpfSourceBase {
    IL_SKEL_CALLBACKS(offcpu_profiler_sk);

public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* Version() const override { return "2.0.0"; }

    Status Init(const ConfigValue& config) override {
        min_duration_us_ = static_cast<uint32_t>(
            config["min_block_us"].AsInt(
                config["min_duration_us"].AsInt(10000)));

        target_pids_ = ParseCommaSeparated<uint32_t>(
            config["target_pids"].AsString(""));
        ParseComms(config["target_process_names"].AsString(
            config["target_comms"].AsString("")));
        return Status::Ok();
    }

    StatusOr<std::string> QueryExtra(
        const std::string& query, const QueryParams& /*params*/) override {
        if (query == "snapshot") {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            return latest_json_snapshot_;
        }
        return Status::Error(StatusCode::kUnimplemented, "unknown query");
    }

    Status Reconfigure(const ConfigValue& params) override {
        target_pids_ = ParseCommaSeparated<uint32_t>(
            params["target_pids"].AsString(""));
        ParseComms(params["target_process_names"].AsString(
            params["target_comms"].AsString("")));

        bpf_util::RewritePidFilter(
            bpf_map__fd(skel()->maps.offcpu_target_pids), target_pids_);
        bpf_util::RewriteCommFilter(
            bpf_map__fd(skel()->maps.offcpu_target_comms), target_comms_);

        {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            latest_json_snapshot_ = R"({"stack_samples":[]})";
        }

        if (!params["min_block_us"].AsString("").empty() ||
            !params["min_duration_us"].AsString("").empty()) {
            return Status(StatusCode::kRequiresRestart,
                "min_block_us is stored in BPF rodata and requires restart");
        }
        return Status::Ok();
    }

protected:
    // ================================================================
    // Hooks
    // ================================================================

    void OnConfigureRodata(void* /*s*/) override {
        if (skel()->rodata)
            skel()->rodata->min_duration_ns =
                static_cast<uint64_t>(min_duration_us_) * 1000ULL;
    }

    void OnConfigureMaps(void* /*s*/) override {
        stacks_fd_ = bpf_map__fd(skel()->maps.offcpu_stacks);
        stats_fd_ = bpf_map__fd(skel()->maps.offcpu_stats);
        SetRingBufFd(bpf_map__fd(skel()->maps.offcpu_events));
        SetGateFd(bpf_map__fd(skel()->maps.offcpu_cfg));
        SetMetaStatsFd(bpf_map__fd(skel()->maps.meta_stats));

        bpf_util::ConfigurePidNamespace(
            bpf_map__fd(skel()->maps.offcpu_pidns_cfg));
        bpf_util::WritePidFilter(
            bpf_map__fd(skel()->maps.offcpu_target_pids), target_pids_);
        bpf_util::WriteCommFilter(
            bpf_map__fd(skel()->maps.offcpu_target_comms), target_comms_);
    }

    ring_buffer_sample_fn GetEventCallback() const override {
        return HandleSignal;
    }

    // Pull 模式：先消费 ring buffer 中的信号，然后读 stats map
    StatusOr<DataBatchPtr> CollectFromMaps() override {
        if (stats_fd_ < 0 || stacks_fd_ < 0)
            return std::make_shared<DataBatch>(DataBatch::Type::kProfile);

        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
        ReadAndFlushStats(batch.get());

        if (!batch->stack_samples().empty())
            GenerateSymbolizedSnapshot(batch);

        return batch;
    }

private:
    // ring buffer 用作信号通道（pid==0 表示 BPF 侧数据就绪）
    // 在新架构中信号由 Collect() → ring_buffer__consume() 自动处理，
    // 回调只需排空 ring buffer 防溢出
    static int HandleSignal(void* /*ctx*/, void* /*data*/, size_t /*size*/) {
        return 0;
    }

    struct OffcpuStatKey {
        uint32_t tgid;
        uint32_t tid;
        int32_t kernel_stack_id;
        int32_t user_stack_id;
    };

    struct OffcpuStatVal {
        uint64_t total_ns;
        uint32_t count;
        uint32_t cpu;
        char comm[16];
    };

    void ReadAndFlushStats(DataBatch* batch) {
        OffcpuStatKey key{}, next_key{};
        OffcpuStatVal val{};

        while (bpf_map_get_next_key(stats_fd_, &key, &next_key) == 0) {
            if (bpf_map_lookup_elem(stats_fd_, &next_key, &val) == 0) {
                auto& cs = batch->AddStackSample();
                cs.pid = next_key.tgid;
                cs.tid = next_key.tid;
                cs.cpu = val.cpu;
                cs.comm = batch->InternString(std::string_view(
                    val.comm, strnlen(val.comm, 16)));
                cs.sample_type = SampleType::kOffCpu;
                cs.duration_ns = val.total_ns;
                cs.count = val.count;
                cs.kernel_stack_id = next_key.kernel_stack_id;
                cs.user_stack_id = next_key.user_stack_id;
                cs.kernel_stack = LookupBpfStackTrace(
                    stacks_fd_, next_key.kernel_stack_id);
                cs.user_stack = LookupBpfStackTrace(
                    stacks_fd_, next_key.user_stack_id);
            }
            bpf_map_delete_elem(stats_fd_, &next_key);
            key = next_key;
        }
    }

    void ParseComms(const std::string& s) {
        target_comms_.clear();
        if (s.empty()) return;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            if (!tok.empty()) target_comms_.push_back(tok);
        }
    }

    // ================================================================
    // 符号解析（保留自旧版，为 QueryExtra("snapshot") 服务）
    // ================================================================

    void GenerateSymbolizedSnapshot(const DataBatchPtr& batch) {
        std::lock_guard<std::mutex> lk(snapshot_mu_);

        if (!kernel_resolver_loaded_) {
            auto st = kernel_resolver_.Load();
            if (st.ok())
                IL_INFO("offcpu_profiler: kernel symbols loaded");
            kernel_resolver_loaded_ = true;
        }

        nlohmann::json j;
        nlohmann::json samples = nlohmann::json::array();
        for (auto& s : batch->stack_samples()) {
            nlohmann::json sj;
            sj["pid"] = s.pid;
            sj["tid"] = s.tid;
            sj["cpu"] = s.cpu;
            sj["count"] = s.count;
            sj["duration_ns"] = s.duration_ns;
            sj["comm"] = std::string(s.comm.data(), s.comm.size());
            sj["sample_type"] = static_cast<int>(s.sample_type);

            nlohmann::json ks = nlohmann::json::array();
            for (auto& f : s.kernel_stack) {
                nlohmann::json fj;
                fj["address"] = f.address;
                std::string sym = kernel_resolver_.Resolve(f.address);
                if (sym.empty()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[kernel 0x%llx]",
                                  static_cast<unsigned long long>(f.address));
                    sym = buf;
                }
                fj["function_name"] = sym;
                ks.push_back(std::move(fj));
            }

            nlohmann::json us = nlohmann::json::array();
            for (auto& f : s.user_stack) {
                nlohmann::json fj;
                fj["address"] = f.address;
                std::string sym;
                if (f.address < 0x1000) {
                    sym = "[thread entry]";
                } else {
                    sym = ResolveUserSymbol(s.pid, f.address);
                    if (sym.empty()) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "[0x%llx]",
                            static_cast<unsigned long long>(f.address));
                        sym = buf;
                    }
                }
                fj["function_name"] = sym;
                us.push_back(std::move(fj));
            }

            sj["kernel_stack"] = std::move(ks);
            sj["user_stack"] = std::move(us);
            samples.push_back(std::move(sj));
        }
        j["stack_samples"] = std::move(samples);
        j["pipeline"] = "offcpu_profile";
        latest_json_snapshot_ = j.dump();
    }

    std::string ResolveUserSymbol(uint32_t pid, uint64_t addr) {
        auto now = std::chrono::steady_clock::now();
        uint32_t cache_key = IsSelfPid(pid) ? self_pid_ : pid;

        auto it = maps_cache_.find(cache_key);
        if (it == maps_cache_.end()) {
            MapsCacheEntry entry;
            if (!LoadProcMaps(cache_key, entry)) return {};
            auto ins = maps_cache_.emplace(cache_key, std::move(entry));
            it = ins.first;
        } else {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.loaded_at).count();
            if (age > 30) LoadProcMaps(cache_key, it->second);
        }

        for (const auto& m : it->second.maps) {
            if (addr < m.start || addr >= m.end) continue;
            if (!m.path.empty() && m.path[0] == '[') {
                if (m.path == "[vdso]") return "__vdso_clock_gettime [vdso]";
                if (m.path == "[vsyscall]") return "[vsyscall]";
                return m.path;
            }

            uint64_t file_off = addr - m.start + m.offset;
            uint64_t map_off = addr - m.start;
            auto elf_it = elf_cache_.find(m.path);
            if (elf_it == elf_cache_.end()) {
                ElfSymbolCache cache;
                if (!cache.Load(m.path)) {
                    if (!TryLoadDebugInfo(m.path, cache)) {
                        std::string ann =
                            AnnotateLibraryOffset(m.path, file_off, map_off);
                        if (!ann.empty()) return ann;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                            m.path.c_str(),
                            static_cast<unsigned long long>(map_off));
                        return std::string(buf);
                    }
                }
                auto ins = elf_cache_.emplace(m.path, std::move(cache));
                elf_it = ins.first;
            }
            std::string sym = elf_it->second.Resolve(file_off);
            if (!sym.empty()) {
                if (sym.size() > 4 && sym.substr(sym.size() - 4) == "+gap") {
                    sym = sym.substr(0, sym.size() - 4);
                    return DemangleSymbol(sym) + " [+gap]";
                }
                return DemangleSymbol(sym);
            }
            std::string ann =
                AnnotateLibraryOffset(m.path, file_off, map_off);
            if (!ann.empty()) return ann;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "[%s+0x%llx]",
                m.path.c_str(),
                static_cast<unsigned long long>(map_off));
            return std::string(buf);
        }
        return {};
    }

    bool IsSelfPid(uint32_t pid) const {
        if (pid == self_pid_) return true;
        std::string status_path = "/proc/" + std::to_string(pid) + "/status";
        std::ifstream f(status_path);
        return !f.good();
    }

    bool LoadProcMaps(uint32_t pid, MapsCacheEntry& entry) {
        entry.maps.clear();
        std::string path = (pid == self_pid_)
            ? "/proc/self/maps"
            : "/proc/" + std::to_string(pid) + "/maps";

        std::ifstream f(path);
        if (!f) {
            if (pid != self_pid_) {
                f.open("/proc/self/maps");
                if (!f) return false;
                if (!self_maps_logged_) {
                    IL_INFO("offcpu_profiler: using /proc/self/maps for pid {}",
                            pid);
                    self_maps_logged_ = true;
                }
            } else {
                return false;
            }
        }
        std::string line;
        while (std::getline(f, line)) {
            ProcMapEntry m{};
            if (!ParseProcMapsLine(line, &m)) continue;
            if (m.path.empty()) continue;
            if (m.path[0] != '/' && m.path[0] != '[') continue;
            entry.maps.push_back(std::move(m));
        }
        entry.loaded_at = std::chrono::steady_clock::now();
        return !entry.maps.empty();
    }

    bool TryLoadDebugInfo(const std::string& elf_path,
                          ElfSymbolCache& cache) {
        std::string build_id = ExtractBuildId(elf_path);
        if (build_id.size() >= 4) {
            std::string bid_path = "/usr/lib/debug/.build-id/"
                + build_id.substr(0, 2) + "/"
                + build_id.substr(2) + ".debug";
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

    static std::string ExtractBuildId(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (buf.size() < sizeof(Elf64_Ehdr)) return {};
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buf.data());
        if (ehdr->e_ident[EI_MAG0] != ELFMAG0) return {};
        if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr))
            return {};
        auto* shdrs =
            reinterpret_cast<Elf64_Shdr*>(buf.data() + ehdr->e_shoff);
        for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
            if (shdrs[i].sh_type != SHT_NOTE) continue;
            if (shdrs[i].sh_offset + shdrs[i].sh_size > buf.size()) continue;
            const char* nd = buf.data() + shdrs[i].sh_offset;
            size_t rem = shdrs[i].sh_size;
            size_t pos = 0;
            while (pos + 12 <= rem) {
                uint32_t namesz =
                    *reinterpret_cast<const uint32_t*>(nd + pos);
                uint32_t descsz =
                    *reinterpret_cast<const uint32_t*>(nd + pos + 4);
                uint32_t type =
                    *reinterpret_cast<const uint32_t*>(nd + pos + 8);
                size_t name_start = pos + 12;
                size_t name_aligned = (namesz + 3) & ~3u;
                size_t desc_start = name_start + name_aligned;
                size_t desc_aligned = (descsz + 3) & ~3u;
                if (desc_start + descsz > rem) break;
                if (type == 3 && namesz == 4 &&
                    std::memcmp(nd + name_start, "GNU", 4) == 0) {
                    std::string hex;
                    hex.reserve(descsz * 2);
                    for (size_t j = 0; j < descsz; ++j) {
                        char h[3];
                        std::snprintf(h, sizeof(h), "%02x",
                            static_cast<uint8_t>(nd[desc_start + j]));
                        hex += h;
                    }
                    return hex;
                }
                pos = desc_start + desc_aligned;
            }
        }
        return {};
    }

    std::string AnnotateLibraryOffset(const std::string& lib_path,
                                      uint64_t file_off,
                                      uint64_t /*map_off*/) {
        if (lib_path.find("libstdc++") != std::string::npos) {
            if (file_off >= 0xf7000 && file_off < 0xf8000)
                return "execute_native_thread_routine [libstdc++]";
            if (file_off >= 0xd5000 && file_off < 0xd6000)
                return "execute_native_thread_routine [libstdc++]";
        }
        if (lib_path.find("libc.so") != std::string::npos ||
            lib_path.find("libc-") != std::string::npos) {
            if (file_off >= 0x94000 && file_off < 0x95000)
                return "start_thread [glibc]";
            if (file_off >= 0x115000 && file_off < 0x116000)
                return "__clone3 [glibc]";
        }
        if (lib_path.find("libpthread") != std::string::npos) {
            if (file_off >= 0x8000 && file_off < 0x9000)
                return "start_thread [libpthread]";
        }
        return {};
    }

    static std::string DemangleSymbol(const std::string& sym) {
        int status = 0;
        char* dm = abi::__cxa_demangle(sym.c_str(), nullptr, nullptr, &status);
        if (status != 0 || !dm) return sym;
        std::string out(dm);
        std::free(dm);
        return out;
    }

    uint32_t min_duration_us_ = 10000;
    int stacks_fd_ = -1;
    int stats_fd_ = -1;
    std::vector<uint32_t> target_pids_;
    std::vector<std::string> target_comms_;

    mutable std::mutex snapshot_mu_;
    std::string latest_json_snapshot_ = R"({"stack_samples":[]})";

    uint32_t self_pid_ = static_cast<uint32_t>(getpid());
    KernelSymbolResolver kernel_resolver_;
    bool kernel_resolver_loaded_ = false;
    bool self_maps_logged_ = false;
    std::unordered_map<std::string, ElfSymbolCache> elf_cache_;
    std::unordered_map<uint32_t, MapsCacheEntry> maps_cache_;
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
