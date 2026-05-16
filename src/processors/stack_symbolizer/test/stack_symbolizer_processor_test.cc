// stack_symbolizer_processor_test.cc — StackSymbolizerProcessor 与 ParseProcMapsLine 单元测试

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "core/common/config.h"
#include "core/engine/data_batch.h"
#include "processors/stack_symbolizer/stack_symbolizer.h"

namespace illuminator {
namespace {

// ParseProcMapsLine：解析典型 /proc/<pid>/maps 行，应得到起止地址、偏移与文件路径。
TEST(StackSymbolizerProcessorTest, ParseProcMapsLineParsesTypicalMapsEntry) {
    const std::string line =
        "7f1234000000-7f1234001000 r-xp 00000000 08:01 123456 /usr/lib/libc.so";
    ProcMapEntry m{};
    ASSERT_TRUE(ParseProcMapsLine(line, &m));
    EXPECT_EQ(m.start, 0x7f1234000000ULL);
    EXPECT_EQ(m.end, 0x7f1234001000ULL);
    EXPECT_EQ(m.offset, 0u);
    EXPECT_EQ(m.path, "/usr/lib/libc.so");
}

// ParseProcMapsLine：格式不完整时应返回 false。
TEST(StackSymbolizerProcessorTest, ParseProcMapsLineRejectsMalformedInput) {
    ProcMapEntry m{};
    EXPECT_FALSE(ParseProcMapsLine("not-a-map-line", &m));
}

// Init：应从 ConfigValue 解析 demangle / kernel_symbols / cache_ttl_sec（demangle 关闭通过后续行为验证）。
TEST(StackSymbolizerProcessorTest, InitParsesConfigKeys) {
    StackSymbolizerProcessor proc;
    ConfigValue cfg;
    cfg.Set("demangle", "false");
    cfg.Set("kernel_symbols", "false");
    cfg.Set("cache_ttl_sec", "7");
    cfg.Set("jit_map", "true");
    ASSERT_TRUE(proc.Init(cfg).ok());
}

// Process：空指针输入应直接返回空。
TEST(StackSymbolizerProcessorTest, ProcessReturnsNullptrForNullInput) {
    StackSymbolizerProcessor proc;
    ConfigValue cfg;
    cfg.Set("kernel_symbols", "false");
    ASSERT_TRUE(proc.Init(cfg).ok());
    auto out = proc.Process(nullptr);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), nullptr);
}

// Process：在无堆栈帧时应对同一批次原地返回（shared_ptr 相等）。
TEST(StackSymbolizerProcessorTest, ProcessReturnsSamePointerWhenNoStacksToSymbolize) {
    StackSymbolizerProcessor proc;
    ConfigValue cfg;
    cfg.Set("kernel_symbols", "false");
    ASSERT_TRUE(proc.Init(cfg).ok());
    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    batch->AddStackSample();
    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), batch);
}

// kernel_symbols 关闭且内核帧地址非零时，应退回十六进制占位符并写入 function_name。
TEST(StackSymbolizerProcessorTest, ProcessKernelFallbackWhenKernelSymbolsDisabled) {
    StackSymbolizerProcessor proc;
    ConfigValue cfg;
    cfg.Set("kernel_symbols", "false");
    cfg.Set("demangle", "false");
    ASSERT_TRUE(proc.Init(cfg).ok());

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kProfile);
    auto& s = batch->AddStackSample();
    StackFrame fr;
    fr.address = 0xdeadbeefULL;
    fr.function_name = {};
    s.kernel_stack.push_back(fr);

    auto out = proc.Process(batch);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.value(), batch);
    ASSERT_FALSE(s.kernel_stack.empty());
    EXPECT_FALSE(s.kernel_stack[0].function_name.empty());
    EXPECT_NE(s.kernel_stack[0].function_name.find("kernel"), std::string_view::npos);
}

// 处理器名称应为 stack_symbolizer。
TEST(StackSymbolizerProcessorTest, ReportsExpectedNameAndVersion) {
    StackSymbolizerProcessor proc;
    EXPECT_STREQ(proc.Name(), "stack_symbolizer");
    EXPECT_STREQ(proc.Version(), "0.1.0");
}

}  // namespace
}  // namespace illuminator
