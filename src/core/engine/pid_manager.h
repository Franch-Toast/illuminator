// ============================================================================
// PidManager — 动态 PID 管理器
// ============================================================================
//
// 职责：
//   1. 维护目标 PID 列表（手动指定 + 按进程名自动发现）
//   2. 定时扫描 /proc 发现进程名匹配的新 PID
//   3. 同步更新 BPF map（运行时动态过滤）
//   4. 支持 API 动态增删 PID
//
// 线程安全：
//   所有公开方法通过 mutex_ 保护内部状态。
//   BPF map 操作是原子的，但 PID 列表变更需要加锁。
//
// 非 BPF 环境：
//   bpf_map_fd_ = -1 时，SyncBpfMap() 为空操作。
//   纯逻辑测试可在此模式下运行。
//
// 条件编译：
//   当 <bpf/bpf.h> 不可用时，BPF 操作被编译为 no-op stub。
// ============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/timer_wheel.h"

// ---- 条件编译：BPF 支持 ----
// 当 <bpf/bpf.h> 可用时包含真实 BPF API；
// 不可用时 BPF 操作编译为空（通过 #if 守卫）。
#if __has_include(<bpf/bpf.h>)
#include <bpf/bpf.h>
#define IL_HAS_BPF_SUPPORT 1
#else
#define IL_HAS_BPF_SUPPORT 0
#endif

namespace illuminator {

class PidManager {
public:
    PidManager(int bpf_map_fd, const std::string& feature_name)
        : bpf_map_fd_(bpf_map_fd), feature_name_(feature_name) {}

    ~PidManager() {
        // scan_timer_id_ 的取消由调用者通过 UnregisterAutoDiscovery 处理
    }

    // ---- 设置目标 PID 列表（完全替换） ----
    // 替换当前 target_pids_ 并同步到 BPF map。
    Status SetTargetPids(const std::vector<int32_t>& pids) {
        std::lock_guard<std::mutex> lock(mutex_);
        target_pids_ = pids;
        SyncBpfMapLocked();
        IL_INFO("{}: SetTargetPids (count={})", feature_name_, target_pids_.size());
        return Status::Ok();
    }

    // ---- 设置按进程名匹配（启用自动发现） ----
    // 设置目标进程名列表，ScanProc() 会定期在 /proc 中查找匹配的新 PID。
    Status SetTargetProcessNames(const std::vector<std::string>& names) {
        std::lock_guard<std::mutex> lock(mutex_);
        target_names_ = names;
        // 立即执行一次扫描
        auto discovered = ScanProcForNamesLocked(names);
        if (!discovered.empty()) {
            for (auto pid : discovered) {
                bool already_known = false;
                for (auto existing : discovered_pids_) {
                    if (existing == pid) { already_known = true; break; }
                }
                if (!already_known) {
                    discovered_pids_.push_back(pid);
                    new_pids_found_++;
                }
            }
            SyncBpfMapLocked();
        }
        IL_INFO("{}: SetTargetProcessNames (names={}, discovered={})",
                feature_name_, target_names_.size(), discovered_pids_.size());
        return Status::Ok();
    }

    // ---- 添加单个 PID ----
    Status AddPid(int32_t pid) {
        if (pid <= 0) {
            return Status::Error(StatusCode::kInvalidArgument, "invalid pid");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // 检查是否已存在
        for (auto p : target_pids_) {
            if (p == pid) return Status::Ok();  // 幂等
        }
        target_pids_.push_back(pid);
        SyncBpfMapLocked();
        IL_INFO("{}: AddPid {}", feature_name_, pid);
        return Status::Ok();
    }

    // ---- 移除单个 PID ----
    Status RemovePid(int32_t pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;

        auto it = std::remove(target_pids_.begin(), target_pids_.end(), pid);
        if (it != target_pids_.end()) {
            target_pids_.erase(it, target_pids_.end());
            changed = true;
        }

        auto dit = std::remove(discovered_pids_.begin(),
                              discovered_pids_.end(), pid);
        if (dit != discovered_pids_.end()) {
            discovered_pids_.erase(dit, discovered_pids_.end());
            changed = true;
        }

        if (changed) {
            SyncBpfMapLocked();
            IL_INFO("{}: RemovePid {}", feature_name_, pid);
        }
        return Status::Ok();
    }

    // ---- 获取当前活跃 PID 列表（target_pids_ + discovered_pids_ 去重） ----
    std::vector<int32_t> GetActivePids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int32_t> result = target_pids_;
        for (auto pid : discovered_pids_) {
            bool found = false;
            for (auto existing : result) {
                if (existing == pid) { found = true; break; }
            }
            if (!found) result.push_back(pid);
        }
        return result;
    }

    // ---- 定时扫描回调（由 TimerWheel 调用） ----
    // 遍历 /proc/[pid]/comm，查找匹配 target_names_ 的新进程。
    void ScanProc() {
        std::lock_guard<std::mutex> lock(mutex_);
        scans_total_++;

        if (target_names_.empty()) return;

        auto new_pids = ScanProcForNamesLocked(target_names_);
        if (new_pids.empty()) return;

        // 检查是否有新发现的 PID
        bool changed = false;
        for (auto pid : new_pids) {
            bool already_known = false;
            for (auto existing : discovered_pids_) {
                if (existing == pid) { already_known = true; break; }
            }
            if (!already_known) {
                discovered_pids_.push_back(pid);
                new_pids_found_++;
                changed = true;
            }
        }

        // 移除已消失的 PID
        auto it = std::remove_if(discovered_pids_.begin(), discovered_pids_.end(),
            [&](int32_t pid) {
                for (auto current : new_pids) {
                    if (current == pid) return false;  // 仍然存在
                }
                return true;  // 已消失
            });
        if (it != discovered_pids_.end()) {
            discovered_pids_.erase(it, discovered_pids_.end());
            changed = true;
        }

        if (changed) {
            SyncBpfMapLocked();
            IL_INFO("{}: ScanProc discovered {} PIDs (total active={})",
                    feature_name_, discovered_pids_.size(), GetActivePidsCountLocked());
        }
    }

    // ---- 注册定时自动发现（通过 TimerWheel） ----
    void RegisterAutoDiscovery(TimerWheel& tw,
                                std::chrono::seconds interval =
                                    std::chrono::seconds(30)) {
        if (scan_timer_id_ != 0) return;  // 已注册
        scan_timer_id_ = tw.AddRepeating(
            std::chrono::duration_cast<std::chrono::milliseconds>(interval),
            [this] { ScanProc(); });
        IL_INFO("{}: auto-discovery registered (interval={}s, timer_id={})",
                feature_name_, interval.count(), scan_timer_id_);
    }

    // ---- 取消定时自动发现 ----
    void UnregisterAutoDiscovery(TimerWheel& tw) {
        if (scan_timer_id_ == 0) return;
        tw.Cancel(scan_timer_id_);
        IL_INFO("{}: auto-discovery unregistered (timer_id={})",
                feature_name_, scan_timer_id_);
        scan_timer_id_ = 0;
    }

    // ---- 统计信息 ----
    uint64_t ScansTotal() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scans_total_;
    }

    uint64_t NewPidsFound() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return new_pids_found_;
    }

    int bpf_map_fd() const { return bpf_map_fd_; }

private:
    // ---- 同步 PID 列表到 BPF map（调用者持锁） ----
    // 策略：先清空 BPF map 中所有旧条目，再写入当前 target_pids_ + discovered_pids_。
    void SyncBpfMapLocked() {
        if (bpf_map_fd_ < 0) return;  // 非 BPF 模式，跳过

#if IL_HAS_BPF_SUPPORT
        // 1. 清空 BPF map 中所有现有条目
        uint32_t cur_key = 0, next_key = 0;
        std::vector<uint32_t> old_keys;
        int err = bpf_map_get_next_key(bpf_map_fd_, nullptr, &cur_key);
        while (err == 0) {
            old_keys.push_back(cur_key);
            err = bpf_map_get_next_key(bpf_map_fd_, &cur_key, &next_key);
            cur_key = next_key;
        }
        for (auto k : old_keys) {
            bpf_map_delete_elem(bpf_map_fd_, &k);
        }

        // 2. 写入所有活跃 PID（target_pids_ + discovered_pids_，去重）
        uint8_t val = 1;
        for (auto pid : target_pids_) {
            uint32_t key = static_cast<uint32_t>(pid);
            bpf_map_update_elem(bpf_map_fd_, &key, &val, BPF_ANY);
        }
        for (auto pid : discovered_pids_) {
            // 跳过已存在于 target_pids_ 中的
            bool dup = false;
            for (auto tp : target_pids_) {
                if (tp == pid) { dup = true; break; }
            }
            if (!dup) {
                uint32_t key = static_cast<uint32_t>(pid);
                bpf_map_update_elem(bpf_map_fd_, &key, &val, BPF_ANY);
            }
        }
#else
        // 无 BPF 支持：no-op
#endif
    }

    // ---- 扫描 /proc 查找进程名匹配的 PID（调用者持锁） ----
    std::vector<int32_t> ScanProcForNamesLocked(
        const std::vector<std::string>& names) {
        std::vector<int32_t> result;
        if (names.empty()) return result;

        DIR* dir = opendir("/proc");
        if (!dir) return result;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;

            // 过滤非数字目录名
            char* endp;
            long pid = strtol(entry->d_name, &endp, 10);
            if (*endp != '\0' || pid <= 0) continue;

            // 读取 /proc/[pid]/comm
            std::string comm_path = "/proc/" + std::string(entry->d_name) + "/comm";
            std::ifstream f(comm_path);
            if (!f.is_open()) continue;

            std::string comm;
            std::getline(f, comm);
            // 去除可能的尾部换行
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
                comm.pop_back();

            // 检查是否匹配任一目标进程名
            for (const auto& name : names) {
                if (comm == name) {
                    result.push_back(static_cast<int32_t>(pid));
                    break;
                }
            }
        }
        closedir(dir);
        return result;
    }

    size_t GetActivePidsCountLocked() const {
        // 简单去重计数
        size_t count = target_pids_.size();
        for (auto pid : discovered_pids_) {
            bool found = false;
            for (auto tp : target_pids_) {
                if (tp == pid) { found = true; break; }
            }
            if (!found) ++count;
        }
        return count;
    }

    int bpf_map_fd_ = -1;
    std::string feature_name_;
    mutable std::mutex mutex_;
    std::vector<int32_t> target_pids_;       // 手动指定的 PID
    std::vector<std::string> target_names_;   // 按进程名匹配
    std::vector<int32_t> discovered_pids_;    // 自动发现的 PID
    uint32_t scan_timer_id_ = 0;              // TimerWheel 定时器 ID
    uint64_t scans_total_ = 0;                // 总扫描次数
    uint64_t new_pids_found_ = 0;             // 新发现的 PID 总数
};

}  // namespace illuminator
