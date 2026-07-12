// PidManager 单元测试：
//   1. PID 增删逻辑
//   2. GetActivePids 去重
//   3. ScanProc /proc 扫描（真实 /proc 环境）
//   4. BPF map 同步（fd=-1 非 BPF 模式，验证不崩溃）
//   5. 统计计数

#include "core/engine/pid_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace illuminator {
namespace {

// ---- 辅助函数：读取当前进程的 comm ----
std::string ReadSelfComm() {
    std::ifstream f("/proc/self/comm");
    std::string comm;
    std::getline(f, comm);
    // 去除尾部换行
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
        comm.pop_back();
    return comm;
}

// ---- 辅助函数：检查 vector 是否包含某值 ----
bool Contains(const std::vector<int32_t>& v, int32_t val) {
    for (auto x : v) {
        if (x == val) return true;
    }
    return false;
}

// ========================================================================
// 1. 基本增删：AddPid / RemovePid
// ========================================================================
TEST(PidManagerTest, AddAndRemovePid) {
    PidManager mgr(-1, "test_feature");

    EXPECT_TRUE(mgr.AddPid(1234).ok());
    EXPECT_TRUE(mgr.AddPid(5678).ok());

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 2u);
    EXPECT_TRUE(Contains(pids, 1234));
    EXPECT_TRUE(Contains(pids, 5678));

    // 移除
    EXPECT_TRUE(mgr.RemovePid(1234).ok());
    pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 1u);
    EXPECT_FALSE(Contains(pids, 1234));
    EXPECT_TRUE(Contains(pids, 5678));
}

// ========================================================================
// 2. 幂等添加：重复添加相同 PID 不应增长列表
// ========================================================================
TEST(PidManagerTest, AddPidIsIdempotent) {
    PidManager mgr(-1, "test_feature");

    mgr.AddPid(100);
    mgr.AddPid(100);  // 重复
    mgr.AddPid(100);  // 再次重复

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 1u);
    EXPECT_EQ(pids[0], 100);
}

// ========================================================================
// 3. 无效 PID 拒绝
// ========================================================================
TEST(PidManagerTest, RejectInvalidPid) {
    PidManager mgr(-1, "test_feature");

    auto status = mgr.AddPid(0);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);

    auto status2 = mgr.AddPid(-1);
    EXPECT_FALSE(status2.ok());
    EXPECT_EQ(status2.code(), StatusCode::kInvalidArgument);
}

// ========================================================================
// 4. 移除不存在的 PID 是安全的（幂等）
// ========================================================================
TEST(PidManagerTest, RemoveNonExistentPidIsSafe) {
    PidManager mgr(-1, "test_feature");
    mgr.AddPid(100);

    // 移除不存在的 PID 不应报错
    EXPECT_TRUE(mgr.RemovePid(999).ok());

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 1u);
    EXPECT_EQ(pids[0], 100);
}

// ========================================================================
// 5. SetTargetPids 完全替换
// ========================================================================
TEST(PidManagerTest, SetTargetPidsReplacesList) {
    PidManager mgr(-1, "test_feature");
    mgr.AddPid(100);
    mgr.AddPid(200);

    // 完全替换
    mgr.SetTargetPids({300, 400, 500});

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 3u);
    EXPECT_TRUE(Contains(pids, 300));
    EXPECT_TRUE(Contains(pids, 400));
    EXPECT_TRUE(Contains(pids, 500));
    EXPECT_FALSE(Contains(pids, 100));
    EXPECT_FALSE(Contains(pids, 200));
}

// ========================================================================
// 6. GetActivePids 去重：target_pids + discovered_pids 不重复
// ========================================================================
TEST(PidManagerTest, GetActivePidsDeduplicates) {
    PidManager mgr(-1, "test_feature");

    // 设置 target_pids
    mgr.SetTargetPids({100, 200});

    // 设置 target_process_names（会触发一次 ScanProc）
    // 使用一个不存在的进程名，避免发现真实 PID
    mgr.SetTargetProcessNames({"__nonexistent_process_xyz__"});

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 2u);  // 只有 target_pids，没有 discovered
    EXPECT_TRUE(Contains(pids, 100));
    EXPECT_TRUE(Contains(pids, 200));
}

// ========================================================================
// 7. ScanProc 发现真实进程
// ========================================================================
TEST(PidManagerTest, ScanProcDiscoversSelf) {
    PidManager mgr(-1, "test_feature");

    // 获取当前进程的 PID 和 comm
    int32_t self_pid = static_cast<int32_t>(getpid());
    std::string self_comm = ReadSelfComm();
    ASSERT_FALSE(self_comm.empty())
        << "Cannot read /proc/self/comm";

    // 设置目标进程名为当前进程名
    mgr.SetTargetProcessNames({self_comm});

    // 手动触发扫描
    mgr.ScanProc();

    auto pids = mgr.GetActivePids();
    // 应该发现当前进程的 PID
    EXPECT_TRUE(Contains(pids, self_pid))
        << "Expected to find self PID " << self_pid
        << " (comm='" << self_comm << "') in active PIDs";
}

// ========================================================================
// 8. ScanProc 统计计数
// ========================================================================
TEST(PidManagerTest, ScanProcIncrementsStatistics) {
    PidManager mgr(-1, "test_feature");

    // 初始状态
    EXPECT_EQ(mgr.ScansTotal(), 0u);
    EXPECT_EQ(mgr.NewPidsFound(), 0u);

    // 设置目标进程名（触发一次扫描）
    std::string self_comm = ReadSelfComm();
    mgr.SetTargetProcessNames({self_comm});

    // SetTargetProcessNames 内部调用一次 ScanProcForNames
    // 但 scans_total_ 只在 ScanProc() 中递增
    EXPECT_EQ(mgr.ScansTotal(), 0u);

    // 手动扫描
    mgr.ScanProc();
    EXPECT_EQ(mgr.ScansTotal(), 1u);
    EXPECT_GE(mgr.NewPidsFound(), 1u);  // 至少发现了 self

    // 再次扫描，不应增加 new_pids_found（已发现的不会重复计数）
    mgr.ScanProc();
    EXPECT_EQ(mgr.ScansTotal(), 2u);
}

// ========================================================================
// 9. BPF map fd = -1 时 SyncBpfMap 是空操作（不崩溃）
// ========================================================================
TEST(PidManagerTest, SyncBpfMapWithInvalidFdIsNoop) {
    PidManager mgr(-1, "test_feature");

    // 这些操作在 fd=-1 时应该全部安全完成
    EXPECT_TRUE(mgr.SetTargetPids({100, 200, 300}).ok());
    EXPECT_TRUE(mgr.AddPid(400).ok());
    EXPECT_TRUE(mgr.RemovePid(200).ok());

    // 验证状态正确（虽然 BPF map 未更新，内存中的列表应该正确）
    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 3u);  // 100, 300, 400
    EXPECT_TRUE(Contains(pids, 100));
    EXPECT_TRUE(Contains(pids, 300));
    EXPECT_TRUE(Contains(pids, 400));
}

// ========================================================================
// 10. 空 target_names 时 ScanProc 不做任何事
// ========================================================================
TEST(PidManagerTest, ScanProcWithNoTargetNamesIsNoop) {
    PidManager mgr(-1, "test_feature");
    mgr.SetTargetPids({100, 200});

    mgr.ScanProc();
    EXPECT_EQ(mgr.ScansTotal(), 1u);
    EXPECT_EQ(mgr.NewPidsFound(), 0u);

    // target_pids 不受影响
    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 2u);
}

// ========================================================================
// 11. RegisterAutoDiscovery / UnregisterAutoDiscovery 不崩溃
// ========================================================================
TEST(PidManagerTest, AutoDiscoveryRegistration) {
    PidManager mgr(-1, "test_feature");
    TimerWheel tw;

    // 在没有 target_names 时注册自动发现
    mgr.SetTargetProcessNames({"__nonexistent_process_xyz__"});
    mgr.RegisterAutoDiscovery(tw, std::chrono::seconds(60));

    // 取消注册
    mgr.UnregisterAutoDiscovery(tw);

    // 重复取消是安全的
    mgr.UnregisterAutoDiscovery(tw);

    SUCCEED();
}

// ========================================================================
// 12. 混合 target_pids + discovered_pids 去重
// ========================================================================
TEST(PidManagerTest, MixedPidsDeduplication) {
    PidManager mgr(-1, "test_feature");

    // 手动添加 PID 100
    mgr.AddPid(100);

    // 设置进程名匹配，会发现 self PID
    std::string self_comm = ReadSelfComm();
    mgr.SetTargetProcessNames({self_comm});

    // 手动扫描
    mgr.ScanProc();

    int32_t self_pid = static_cast<int32_t>(getpid());
    auto pids = mgr.GetActivePids();

    // 应包含 100 和 self_pid，且不重复
    EXPECT_TRUE(Contains(pids, 100));
    EXPECT_TRUE(Contains(pids, self_pid));

    // 如果 self_pid == 100，总数应为 1，否则为 2
    if (self_pid != 100) {
        EXPECT_EQ(pids.size(), 2u);
    }
}

// ========================================================================
// 13. RemovePid 同时从 discovered_pids 中移除
// ========================================================================
TEST(PidManagerTest, RemovePidAlsoRemovesFromDiscovered) {
    PidManager mgr(-1, "test_feature");

    // 设置进程名匹配，发现 self PID
    std::string self_comm = ReadSelfComm();
    mgr.SetTargetProcessNames({self_comm});
    mgr.ScanProc();

    int32_t self_pid = static_cast<int32_t>(getpid());
    auto pids = mgr.GetActivePids();
    ASSERT_TRUE(Contains(pids, self_pid));

    // 从 discovered_pids 中移除
    mgr.RemovePid(self_pid);

    pids = mgr.GetActivePids();
    EXPECT_FALSE(Contains(pids, self_pid));
}

// ========================================================================
// 14. 大量 PID 添加/删除性能（基本功能验证）
// ========================================================================
TEST(PidManagerTest, BulkPidsOperations) {
    PidManager mgr(-1, "test_feature");

    std::vector<int32_t> bulk;
    for (int i = 1000; i < 1100; ++i) {
        bulk.push_back(i);
    }
    mgr.SetTargetPids(bulk);

    auto pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 100u);

    // 删除一半
    for (int i = 1000; i < 1050; ++i) {
        mgr.RemovePid(i);
    }

    pids = mgr.GetActivePids();
    ASSERT_EQ(pids.size(), 50u);
    EXPECT_TRUE(Contains(pids, 1050));
    EXPECT_TRUE(Contains(pids, 1099));
    EXPECT_FALSE(Contains(pids, 1000));
}

}  // namespace
}  // namespace illuminator
