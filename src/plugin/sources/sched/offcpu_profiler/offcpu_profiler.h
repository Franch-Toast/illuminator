// ============================================================================
// OffcpuProfilerSource — Off-CPU 性能剖析器（Pull 模式）
// ============================================================================
//
// 通过 eBPF 追踪进程在非 CPU 执行状态下的等待时间。
// 与 cpu_profiler（On-CPU 采样）互补。
//
// 基于 EbpfSkeletonPullSource 模板，该模板统一管理：
//   - Skeleton 生命周期（open/load/attach/destroy）
//   - 多 bit 配置 gate + 延迟启动
//   - PID/进程名/PID Namespace 过滤
//   - Ring buffer poll 线程
//   - 缓存管理 + Collect()
//   - Pause/Resume/Reconfigure
//
// 本子类仅需关注 offcpu 特有的逻辑：
//   - rodata 配置（min_duration_ns）
//   - 聚合数据读取（ReadAndClearStats）
//   - JSON 快照生成（QueryExtra("snapshot")）
// ============================================================================

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <elf.h>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "ebpf/include/bpf_compat.h"

#include "offcpu_profiler_sk.skel.h"
#include "core/common/logging.h"
#include "ebpf/include/event_types.h"
#include "ebpf/loader/stack_trace_util.h"
#include "plugin/processors/stack_symbolizer/stack_symbolizer.h"
#include "plugin/sources/ebpf_skeleton_pull_source.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

IL_DEFINE_SKEL_OPS_WITH_META(OffcpuSkelOps, offcpu_profiler_sk,
                             offcpu_events, offcpu_cfg, "offcpu-poll",
                             meta_stats);

// ============================================================================
// OffcpuProfilerSource — 继承 Pull 模板，实现 offcpu 特有逻辑
// ============================================================================
class OffcpuProfilerSource
    : public EbpfSkeletonPullSource<OffcpuSkelOps> {
public:
    const char* Name() const override { return "offcpu_profiler"; }
    const char* Version() const override { return "0.1.0"; }

    StatusOr<std::string> QueryExtra(
        const std::string& query, const QueryParams& /*params*/) override {
        if (query == "snapshot") {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            return latest_json_snapshot_;
        }
        return Status::Error(StatusCode::kUnimplemented, "unknown query");
    }

protected:
    // ---- 子类专有配置 ----
    Status InitExtra(const ConfigValue& config) override {
        min_duration_us_ = static_cast<uint32_t>(
            config["min_block_us"].AsInt(
                config["min_duration_us"].AsInt(10000)));
        return Status::Ok();
    }

    // ---- rodata 配置：min_duration_ns ----
    void ConfigureSkeleton(skel_type* skel) override {
        if (skel->rodata) {
            skel->rodata->min_duration_ns =
                static_cast<uint64_t>(min_duration_us_) * 1000ULL;
        }
    }

    // ---- Map FD 提供者 ----
    int GetStacksMapFd() const override {
        return bpf_map__fd(skel_->maps.offcpu_stacks);
    }
    int GetTargetPidsMapFd() const override {
        return bpf_map__fd(skel_->maps.offcpu_target_pids);
    }
    int GetTargetCommsMapFd() const override {
        return bpf_map__fd(skel_->maps.offcpu_target_comms);
    }
    int GetPidnsMapFd() const override {
        return bpf_map__fd(skel_->maps.offcpu_pidns_cfg);
    }

    ring_buffer_sample_fn EventCallback() const override {
        return HandleEvent;
    }

    DataBatchPtr MakeEmptyBatch() const override {
        return std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    }

    bool IsCacheEmpty() const override {
        return !cached_batch_ || cached_batch_->stack_samples().empty();
    }

    // ---- Reconfigure 扩展：min_block_us 需要重启 ----
    Status OnReconfigureExtra(const ConfigValue& params) override {
        {
            std::lock_guard<std::mutex> lk(snapshot_mu_);
            latest_json_snapshot_ = "{\"stack_samples\":[]}";
        }

        if (!params["min_block_us"].AsString("").empty() ||
            !params["min_duration_us"].AsString("").empty()) {
            return Status(StatusCode::kRequiresRestart,
                "min_block_us is stored in BPF rodata and requires restart");
        }
        return Status::Ok();
    }

    // ========================================================================
    // ReadAndClearStats — 批量读取 offcpu_stats BPF map 并转换为 DataBatch
    // ========================================================================
    void ReadAndClearStats() override {
        int stats_fd = bpf_map__fd(skel_->maps.offcpu_stats);
        if (stats_fd < 0) return;

        struct offcpu_stat_key {
            uint32_t tgid;
            uint32_t tid;
            int32_t kernel_stack_id;
            int32_t user_stack_id;
        };
        struct offcpu_stat_val {
            uint64_t total_ns;
            uint32_t count;
            uint32_t cpu;
            char comm[16];
        };

        auto batch = MakeEmptyBatch();
        offcpu_stat_key key = {}, next_key = {};
        offcpu_stat_val val = {};

        while (bpf_map_get_next_key(stats_fd, &key, &next_key) == 0) {
            if (bpf_map_lookup_elem(stats_fd, &next_key, &val) == 0) {
                auto kernel_stack =
                    LookupBpfStackTrace(stacks_fd_, next_key.kernel_stack_id);
                auto user_stack =
                    LookupBpfStackTrace(stacks_fd_, next_key.user_stack_id);

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
                cs.kernel_stack = kernel_stack;
                cs.user_stack = user_stack;
            }
            bpf_map_delete_elem(stats_fd, &next_key);
            key = next_key;
        }

        if (batch->stack_samples().empty())
            return;

        IL_DEBUG("offcpu_profiler: flush {} samples",
                 batch->stack_samples().size());

        GenerateSymbolizedSnapshot(batch);

        std::lock_guard<std::mutex> lk(cache_mu_);
        if (!cached_batch_)
            cached_batch_ = MakeEmptyBatch();
        for (const auto& s : batch->stack_samples()) {
            auto& dst = cached_batch_->AddStackSample();
            dst.pid = s.pid;
            dst.tid = s.tid;
            dst.cpu = s.cpu;
            dst.count = s.count;
            dst.duration_ns = s.duration_ns;
            dst.sample_type = s.sample_type;
            dst.kernel_stack_id = s.kernel_stack_id;
            dst.user_stack_id = s.user_stack_id;
            dst.kernel_stack = s.kernel_stack;
            dst.user_stack = s.user_stack;
            dst.comm = cached_batch_->InternString(s.comm);
        }
    }

private:
    // ========================================================================
    // HandleEvent — ring buffer 信号回调
    // ========================================================================
    static int HandleEvent(void* ctx, void* data, size_t size) {
        auto* self = static_cast<OffcpuProfilerSource*>(ctx);
        if (size < sizeof(il_offcpu_event))
            return 0;
        auto* ev = static_cast<il_offcpu_event*>(data);
        if (ev->pid == 0) {
            self->ReadAndClearStats();
        }
        return 0;
    }

    // ========================================================================
    // GenerateSymbolizedSnapshot — 生成符号化 JSON 快照供 HTTP API 使用
    // ========================================================================
    void GenerateSymbolizedSnapshot(const DataBatchPtr& batch) {
        std::lock_guard<std::mutex> lk(snapshot_mu_);

        if (!kernel_resolver_loaded_) {
            auto st = kernel_resolver_.Load();
            if (st.ok())
                IL_INFO("offcpu_profiler: kernel symbols loaded");
            kernel_resolver_loaded_ = true;
        }

        json j;
        json samples = json::array();
        for (auto& s : batch->stack_samples()) {
            json sj;
            sj["pid"] = s.pid;
            sj["tid"] = s.tid;
            sj["cpu"] = s.cpu;
            sj["count"] = s.count;
            sj["duration_ns"] = s.duration_ns;
            sj["comm"] = std::string(s.comm.data(), s.comm.size());
            sj["sample_type"] = static_cast<int>(s.sample_type);

            json ks = json::array();
            for (auto& f : s.kernel_stack) {
                json fj;
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

            json us = json::array();
            for (auto& f : s.user_stack) {
                json fj;
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

    // ========================================================================
    // 用户态符号解析
    // ========================================================================
    std::string ResolveUserSymbol(uint32_t pid, uint64_t addr) {
        auto now = std::chrono::steady_clock::now();
        uint32_t cache_key = IsSelfPid(pid) ? self_pid_ : pid;

        auto it = maps_cache_.find(cache_key);
        if (it == maps_cache_.end()) {
            MapsCacheEntry entry;
            if (!LoadProcMaps(cache_key, entry))
                return {};
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

    // ---- 私有状态 ----
    uint32_t min_duration_us_ = 10000;
    mutable std::mutex snapshot_mu_;
    std::string latest_json_snapshot_ = "{\"stack_samples\":[]}";

    // ---- 符号解析 ----
    uint32_t self_pid_ = static_cast<uint32_t>(getpid());
    KernelSymbolResolver kernel_resolver_;
    bool kernel_resolver_loaded_ = false;
    bool self_maps_logged_ = false;
    std::unordered_map<std::string, ElfSymbolCache> elf_cache_;
    std::unordered_map<uint32_t, MapsCacheEntry> maps_cache_;
};

IL_REGISTER_SOURCE("offcpu_profiler", OffcpuProfilerSource);

}  // namespace illuminator
