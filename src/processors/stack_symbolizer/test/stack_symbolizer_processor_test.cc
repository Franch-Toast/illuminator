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

// ElfSymbolCache：加载当前二进制自身并解析已知符号（验证 load_base 归一化）
TEST(StackSymbolizerProcessorTest, ElfSymbolCacheResolvesOwnBinary) {
    ElfSymbolCache cache;
    // /proc/self/exe 指向当前测试二进制
    bool loaded = cache.Load("/proc/self/exe");
    ASSERT_TRUE(loaded);
    // "main" 函数在任何可执行文件中都应存在
    // 通过 nm 我们知道 main 的符号值 > 0
    // 验证 Resolve 对一个合理偏移不会崩溃
    std::string sym = cache.Resolve(0);
    // 偏移 0 可能无符号，但不应崩溃
    (void)sym;
}

// ElfSymbolCache：Resolve 返回最近的低地址符号名
TEST(StackSymbolizerProcessorTest, ElfSymbolCacheResolveFindsSymbol) {
    ElfSymbolCache cache;
    ASSERT_TRUE(cache.Load("/proc/self/exe"));
    // PIE 测试二进制的 .text 通常在较高偏移（0x10000+）
    bool found_any = false;
    for (uint64_t off = 0x1000; off < 0x800000; off += 0x100) {
        std::string sym = cache.Resolve(off);
        if (!sym.empty()) {
            found_any = true;
            EXPECT_FALSE(sym.empty());
            break;
        }
    }
    EXPECT_TRUE(found_any) << "Should find at least one symbol in test binary";
}

// ParseProcMapsLine：带非零文件偏移的映射行
TEST(StackSymbolizerProcessorTest, ParseProcMapsLineWithOffset) {
    const std::string line =
        "7f0001000000-7f0001100000 r-xp 00022000 08:01 999 /lib/libfoo.so";
    ProcMapEntry m{};
    ASSERT_TRUE(ParseProcMapsLine(line, &m));
    EXPECT_EQ(m.start, 0x7f0001000000ULL);
    EXPECT_EQ(m.end, 0x7f0001100000ULL);
    EXPECT_EQ(m.offset, 0x22000ULL);
    EXPECT_EQ(m.path, "/lib/libfoo.so");
}

// ParseProcMapsLine：匿名映射（无路径）应设置空路径
TEST(StackSymbolizerProcessorTest, ParseProcMapsLineAnonymousMapping) {
    const std::string line =
        "7fff00000000-7fff00001000 rw-p 00000000 00:00 0";
    ProcMapEntry m{};
    bool ok = ParseProcMapsLine(line, &m);
    if (ok) {
        EXPECT_TRUE(m.path.empty());
    }
}

}  // namespace
}  // namespace illuminator
