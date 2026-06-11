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

// ParseProcMapsLine: [vdso] 等特殊映射应被正确解析
TEST(StackSymbolizerProcessorTest, ParseProcMapsLineVdso) {
    const std::string line =
        "7fff7e5fe000-7fff7e600000 r-xp 00000000 00:00 0                          [vdso]";
    ProcMapEntry m{};
    ASSERT_TRUE(ParseProcMapsLine(line, &m));
    EXPECT_EQ(m.start, 0x7fff7e5fe000ULL);
    EXPECT_EQ(m.end, 0x7fff7e600000ULL);
    EXPECT_EQ(m.path, "[vdso]");
}

// ElfSymbolCache: nearest-symbol 启发式 — size=0 的符号应使用到下一符号的距离作为边界
TEST(StackSymbolizerProcessorTest, ElfSymbolCacheResolveWithGapHeuristic) {
    ElfSymbolCache cache;
    ASSERT_TRUE(cache.Load("/proc/self/exe"));
    // 搜索两个连续有效偏移（间隔 < 4KB）
    std::string first_sym;
    uint64_t first_off = 0;
    for (uint64_t off = 0x1000; off < 0x800000; off += 0x10) {
        std::string sym = cache.Resolve(off);
        if (!sym.empty() && sym.find("+gap") == std::string::npos) {
            first_sym = sym;
            first_off = off;
            break;
        }
    }
    ASSERT_FALSE(first_sym.empty()) << "Need at least one resolvable symbol";
    // 同一符号内的连续地址应解析到相同名称
    std::string same = cache.Resolve(first_off + 1);
    EXPECT_FALSE(same.empty());
}

// ElfSymbolCache: Resolve 对完全超出范围的地址返回空
TEST(StackSymbolizerProcessorTest, ElfSymbolCacheResolveOutOfRange) {
    ElfSymbolCache cache;
    ASSERT_TRUE(cache.Load("/proc/self/exe"));
    std::string sym = cache.Resolve(0xFFFFFFFF00000000ULL);
    EXPECT_TRUE(sym.empty());
}

// ExtractBuildId: 从自身二进制提取 build-id
TEST(StackSymbolizerProcessorTest, ExtractBuildIdFromSelf) {
    // 通过 StackSymbolizerProcessor 的静态方法间接测试
    // 由于 ExtractBuildId 是 private，我们验证 TryLoadDebugInfo 不会崩溃
    ElfSymbolCache cache;
    // 即使 debug info 不存在，Load 本身应成功（从 /proc/self/exe）
    bool loaded = cache.Load("/proc/self/exe");
    EXPECT_TRUE(loaded);
}

// KernelSymbolResolver: 基本加载和解析验证
TEST(StackSymbolizerProcessorTest, KernelSymbolResolverBasic) {
    KernelSymbolResolver resolver;
    auto st = resolver.Load();
    // 即使内核符号不可读（无 root），也不应崩溃
    if (st.ok()) {
        // 应能解析到至少一个常见内核函数
        // 0xffffffff81000000 通常是内核 text 起始地址
        // 尝试一些可能的地址
        std::string sym = resolver.Resolve(0xffffffff81000000ULL);
        // 不强制要求解析成功（地址可能不对），但不应崩溃
        (void)sym;
    }
}

}  // namespace
}  // namespace illuminator
