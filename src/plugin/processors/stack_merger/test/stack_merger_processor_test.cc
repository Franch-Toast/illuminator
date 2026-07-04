// stack_merger_processor_test.cc — StackMergerProcessor 单元测试

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/common/data_batch.h"
#include "plugin/processors/stack_merger/stack_merger.h"

namespace illuminator {
namespace {

void AddUserFrame(DataBatch& batch, StackSample& s, const char* fn) {
    StackFrame fr;
    fr.function_name = batch.InternString(fn);
    s.user_stack.push_back(fr);
}

void AddKernelFrame(DataBatch& batch, StackSample& s, const char* fn) {
    StackFrame fr;
    fr.function_name = batch.InternString(fn);
    s.kernel_stack.push_back(fr);
}

// 默认 group_by=comm：相同 comm 且用户栈一致时应合并为一条并累加 count。
TEST(StackMergerProcessorTest, DefaultGroupByCommMergesIdenticalUserStacks) {
    StackMergerProcessor proc;
    ASSERT_TRUE(proc.Init(ConfigValue()).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    for (int i = 0; i < 2; ++i) {
        auto& s = batch->AddStackSample();
        s.comm = batch->InternString("app");
        s.pid = 100;
        s.tid = static_cast<uint32_t>(i);
        s.count = 1;
        AddUserFrame(*batch, s, "main");
    }

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto got = out.value();
    ASSERT_EQ(got->stack_samples().size(), 1u);
    EXPECT_EQ(got->stack_samples()[0].count, 2u);
}

// comm 不同但用户栈相同：按 comm 分组时不应合并。
TEST(StackMergerProcessorTest, DifferentCommSameStackDoesNotMerge) {
    StackMergerProcessor proc;
    ASSERT_TRUE(proc.Init(ConfigValue()).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s1 = batch->AddStackSample();
    s1.comm = batch->InternString("a");
    s1.pid = 1;
    s1.tid = 1;
    s1.count = 1;
    AddUserFrame(*batch, s1, "main");

    auto& s2 = batch->AddStackSample();
    s2.comm = batch->InternString("b");
    s2.pid = 1;
    s2.tid = 1;
    s2.count = 1;
    AddUserFrame(*batch, s2, "main");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value()->stack_samples().size(), 2u);
}

// group_by=pid：忽略 comm 差异，只要 pid 与用户栈一致即合并。
TEST(StackMergerProcessorTest, GroupByPidMergesAcrossDifferentComm) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "pid");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s1 = batch->AddStackSample();
    s1.comm = batch->InternString("p1");
    s1.pid = 42;
    s1.tid = 1;
    s1.count = 2;
    AddUserFrame(*batch, s1, "foo");

    auto& s2 = batch->AddStackSample();
    s2.comm = batch->InternString("p2");
    s2.pid = 42;
    s2.tid = 2;
    s2.count = 3;
    AddUserFrame(*batch, s2, "foo");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto got = out.value();
    ASSERT_EQ(got->stack_samples().size(), 1u);
    EXPECT_EQ(got->stack_samples()[0].pid, 42u);
    EXPECT_EQ(got->stack_samples()[0].count, 5u);
}

// group_by=tid：使用 pid:tid 作为分组前缀，同一线程相同用户栈应合并。
TEST(StackMergerProcessorTest, GroupByTidMergesSamplesForSamePidTidPair) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "tid");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    for (int i = 0; i < 2; ++i) {
        auto& s = batch->AddStackSample();
        s.comm = batch->InternString("app");
        s.pid = 7;
        s.tid = 99;
        s.count = 1;
        AddUserFrame(*batch, s, "bar");
    }

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto got = out.value();
    ASSERT_EQ(got->stack_samples().size(), 1u);
    EXPECT_EQ(got->stack_samples()[0].count, 2u);
}

// group_by=global：忽略 comm/pid/tid，仅按栈内容（及可选内核栈）合并。
TEST(StackMergerProcessorTest, GroupByGlobalMergesIgnoringCommPidTid) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "global");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s1 = batch->AddStackSample();
    s1.comm = batch->InternString("x");
    s1.pid = 1;
    s1.tid = 1;
    s1.count = 1;
    AddUserFrame(*batch, s1, "baz");

    auto& s2 = batch->AddStackSample();
    s2.comm = batch->InternString("y");
    s2.pid = 2;
    s2.tid = 2;
    s2.count = 4;
    AddUserFrame(*batch, s2, "baz");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto got = out.value();
    ASSERT_EQ(got->stack_samples().size(), 1u);
    EXPECT_EQ(got->stack_samples()[0].count, 5u);
}

// include_kernel=false 时内核栈不参与合并键：用户栈相同但内核栈不同仍应合并。
TEST(StackMergerProcessorTest, IncludeKernelFalseIgnoresKernelStackInMergeKey) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "comm");
    cfg.Set("include_kernel", "false");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);

    auto& s1 = batch->AddStackSample();
    s1.comm = batch->InternString("app");
    s1.count = 1;
    AddKernelFrame(*batch, s1, "k1");
    AddUserFrame(*batch, s1, "u1");

    auto& s2 = batch->AddStackSample();
    s2.comm = batch->InternString("app");
    s2.count = 1;
    AddKernelFrame(*batch, s2, "k2");
    AddUserFrame(*batch, s2, "u1");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value()->stack_samples().size(), 1u);
}

// stack_samples 为空时应直接返回输入指针（短路，不新建批次）。
TEST(StackMergerProcessorTest, ReturnsInputUnchangedWhenNoStackSamples) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "global");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    batch->AddRecord();

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), batch);
}

// 多条相同键的采样：count 应做算术累加。
TEST(StackMergerProcessorTest, AccumulatesCountAcrossMergedSamples) {
    StackMergerProcessor proc;
    ConfigValue cfg;
    cfg.Set("group_by", "global");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    for (uint64_t c : {2u, 3u, 5u}) {
        auto& s = batch->AddStackSample();
        s.count = c;
        AddUserFrame(*batch, s, "leaf");
    }

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value()->stack_samples().size(), 1u);
    EXPECT_EQ(out.value()->stack_samples()[0].count, 10u);
}

// Record 列表应原样复制到输出批次，不受堆栈合并影响。
TEST(StackMergerProcessorTest, CopiesRecordsUnmodifiedWhileMergingStacks) {
    StackMergerProcessor proc;
    ASSERT_TRUE(proc.Init(ConfigValue()).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& rec = batch->AddRecord();
    rec.SetField(batch->InternString("n"), int64_t{123});

    auto& s = batch->AddStackSample();
    s.comm = batch->InternString("app");
    s.count = 1;
    AddUserFrame(*batch, s, "f");

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    auto got = out.value();
    ASSERT_EQ(got->records().size(), 1u);
    const auto* fv = got->records()[0].GetField(got->InternString("n"));
    ASSERT_NE(fv, nullptr);
    ASSERT_EQ(std::get<int64_t>(*fv), 123);
}

// 插件名称应为 stack_merger。
TEST(StackMergerProcessorTest, ReportsExpectedName) {
    StackMergerProcessor proc;
    EXPECT_STREQ(proc.Name(), "stack_merger");
}

}  // namespace
}  // namespace illuminator
