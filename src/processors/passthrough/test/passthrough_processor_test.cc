// passthrough_processor_test.cc — PassthroughProcessor 单元测试

#include <memory>

#include <gtest/gtest.h>

#include "core/engine/data_batch.h"
#include "processors/passthrough/passthrough_processor.h"

namespace illuminator {
namespace {

// Process() 应在非空输入时返回与输入完全相同的 shared_ptr（透传零拷贝语义）。
TEST(PassthroughProcessorTest, ProcessReturnsSamePointerForNonNullInput) {
    PassthroughProcessor proc;
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), batch);
}

// Process() 在输入为 nullptr 时应返回成功且值为 nullptr。
TEST(PassthroughProcessorTest, ProcessReturnsNullptrWhenInputIsNull) {
    PassthroughProcessor proc;
    auto out = proc.Process(nullptr);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), nullptr);
}

// Process() 为同一指针时，records 与 stack_samples 内容应保持不变。
TEST(PassthroughProcessorTest, ProcessPreservesAllRecordsAndStackSamples) {
    PassthroughProcessor proc;
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);

    auto& rec = batch->AddRecord();
    rec.labels.push_back({batch->InternString("k"), batch->InternString("v")});
    rec.SetField(batch->InternString("x"), int64_t{42});

    auto& ss = batch->AddStackSample();
    ss.pid = 7;
    ss.tid = 8;
    ss.comm = batch->InternString("comm");
    ss.count = 3;
    StackFrame fr;
    fr.function_name = batch->InternString("fn");
    ss.user_stack.push_back(fr);

    const auto records_before = batch->records();
    const auto stacks_before = batch->stack_samples();

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value(), batch);

    ASSERT_EQ(batch->records().size(), records_before.size());
    ASSERT_EQ(batch->stack_samples().size(), stacks_before.size());
    EXPECT_EQ(batch->records().front().labels.front().key, batch->InternString("k"));
    EXPECT_EQ(batch->records().front().labels.front().value, batch->InternString("v"));
    EXPECT_EQ(batch->stack_samples().front().pid, 7u);
    EXPECT_EQ(batch->stack_samples().front().count, 3u);
}

// Name()/Version() 应与插件契约一致，便于配置与兼容性检查。
TEST(PassthroughProcessorTest, ReportsExpectedNameAndVersion) {
    PassthroughProcessor proc;
    EXPECT_STREQ(proc.Name(), "passthrough");
    EXPECT_STREQ(proc.Version(), "0.1.0");
}

}  // namespace
}  // namespace illuminator
