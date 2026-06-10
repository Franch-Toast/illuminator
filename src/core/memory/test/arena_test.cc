// ============================================================================
// Arena 单元测试 — 校验碰撞指针分配器的对齐、扩展块与统计语义
//
// 覆盖要点：
// - Allocate(size, alignment) 的基本分配与对齐偏移
// - CopyString 将 string_view 拷贝进 Arena 内存并返回可引用的切片
// - Reset 释放扩展块并归零统计，同时可继续从首块分配
// - 超过默认块大小的单次分配触发新块（大块路径）
// - TotalAllocated（总分配字节）与 BlockCount（块数量）统计一致性
// - 多次分配的地址区间互不相交，避免重叠踩踏
// ============================================================================

#include "core/memory/arena.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace illuminator {
namespace {

// 基本分配：Allocate 返回非空指针，对齐到 sizeof(void*) 时能整除地址
TEST(ArenaTest, BasicAllocation_RespectsAlignment) {
    Arena arena;
    void* p1 = arena.Allocate(17, alignof(std::max_align_t));
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p1) % alignof(std::max_align_t), 0U);

    void* p2 = arena.Allocate(3, 4);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p2) % 4, 0U);
}

// CopyString：应将源字符串逐字节拷贝至 Arena 并拥有独立存储
TEST(ArenaTest, CopyString_CopiesBytesIntoArenaBacking) {
    Arena arena;
    const char kLiteral[] = "illuminator arena";
    std::string_view sv(kLiteral);
    std::string_view interned = arena.CopyString(sv);

    ASSERT_EQ(interned.size(), sv.size());
    EXPECT_NE(interned.data(), sv.data());

    std::string_view short_interned = arena.CopyString("illum");
    ASSERT_EQ(short_interned.size(), 5U);
    EXPECT_EQ(short_interned, "illum");
}

// CopyString 返回值与源内容按字节一致
TEST(ArenaTest, CopyString_ResultMatchesSourceContent) {
    Arena arena;
    const std::string source("zero-copy style");
    std::string_view out = arena.CopyString(source);
    EXPECT_EQ(out, std::string_view(source));
    EXPECT_EQ(std::memcmp(out.data(), source.data(), source.size()), 0);
}

// 空字符串不产生分配，返回默认构造的 string_view
TEST(ArenaTest, CopyString_EmptyReturnsEmptyView) {
    Arena arena;
    std::string_view out = arena.CopyString("");
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(out.data(), nullptr);
}

// Reset 后首块保留，可重新分配；统计字段回到初始态
TEST(ArenaTest, Reset_ReleasesExtraBlocksAndAllowsFreshAllocations) {
    Arena arena(Arena::kDefaultBlockSize);

    void* before = arena.Allocate(32, 8);
    ASSERT_NE(before, nullptr);
    const size_t blocks_after_small = arena.BlockCount();
    ASSERT_GE(blocks_after_small, 1U);

    // 触发额外的块以便 Reset 有东西可释放
    void* huge = arena.Allocate(Arena::kDefaultBlockSize + 1, alignof(std::max_align_t));
    ASSERT_NE(huge, nullptr);
    EXPECT_GE(arena.BlockCount(), 2U);

    arena.Reset();

    EXPECT_EQ(arena.BlockCount(), 1U);
    EXPECT_EQ(arena.TotalAllocated(), 0U);

    void* after = arena.Allocate(64, 16);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after, huge);
}

// 大于默认块大小的分配应自动扩展 Arena（新块容量覆盖请求尺寸）
TEST(ArenaTest, OversizedSingleAllocation_AddsNewBlock) {
    Arena arena;
    ASSERT_EQ(arena.BlockCount(), 1U);

    const size_t request = Arena::kDefaultBlockSize + 4096;
    void* p = arena.Allocate(request, alignof(std::max_align_t));
    ASSERT_NE(p, nullptr);
    EXPECT_GE(arena.BlockCount(), 2U);
    EXPECT_GE(arena.TotalAllocated(), request);

    std::memset(p, 0xAB, request);
}

// TotalAllocated 与 BlockCount 与多次分配的规模一致
TEST(ArenaTest, Stats_AccumulateBytesAndTrackBlockGrowth) {
    Arena arena(4096);
    EXPECT_EQ(arena.BlockCount(), 1U);
    EXPECT_EQ(arena.TotalAllocated(), 0U);

    void* a = arena.Allocate(100);
    void* b = arena.Allocate(200, 16);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(arena.TotalAllocated(), 300U);
    EXPECT_GE(arena.BlockCount(), 1U);

    // 塞满首块触顶，促使第二块出现
    arena.Allocate(3900, alignof(std::max_align_t));
    EXPECT_GE(arena.BlockCount(), 2U);
    EXPECT_GT(arena.TotalAllocated(), 300U);
}

// 多次分配返回的区间互不重叠，避免逻辑上发生覆盖
TEST(ArenaTest, RepeatedAllocations_NoOverlappingRanges) {
    Arena arena(512);
    struct Span {
        uint8_t* ptr;
        size_t len;
    };
    std::vector<Span> spans;
    spans.reserve(32);
    for (int i = 0; i < 32; ++i) {
        const size_t len = 16 + static_cast<size_t>(i);
        auto* p = static_cast<uint8_t*>(arena.Allocate(len, 8));
        ASSERT_NE(p, nullptr);
        std::memset(p, static_cast<int>(i), len);
        spans.push_back({p, len});
    }
    for (size_t i = 0; i < spans.size(); ++i) {
        for (size_t j = i + 1; j < spans.size(); ++j) {
            const auto& a = spans[i];
            const auto& b = spans[j];
            const auto a_end = reinterpret_cast<std::uintptr_t>(a.ptr + a.len);
            const auto b_end = reinterpret_cast<std::uintptr_t>(b.ptr + b.len);
            const auto a_begin = reinterpret_cast<std::uintptr_t>(a.ptr);
            const auto b_begin = reinterpret_cast<std::uintptr_t>(b.ptr);
            const bool disjoint = (a_end <= b_begin) || (b_end <= a_begin);
            EXPECT_TRUE(disjoint)
                << "检测到重叠分配：块 " << i << " 与块 " << j << " 地址区间交叉。";
        }
    }
}

// 默认构造使用 64KB 级别的块（常量契约）
TEST(ArenaTest, DefaultBlockSize_Is64KiB) {
    EXPECT_EQ(Arena::kDefaultBlockSize, static_cast<size_t>(64 * 1024));
}

}  // namespace
}  // namespace illuminator
