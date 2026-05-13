// ============================================================================
// Illuminator Arena 内存分配器 — 零拷贝数据流的内存基础
// ============================================================================
//
// Arena（内存池/竞技场）是 Illuminator 实现零拷贝数据流的核心组件。
// 设计灵感来源于 LoongCollector 的 SourceBuffer 设计。
//
// 核心思想：
// ==========
// 传统的字符串处理方式中，每次传递数据都需要拷贝字符串（如 Record 中的
// 标签键值、字段名、函数名等），造成大量内存分配和拷贝开销。
//
// Arena 提供了一种"碰撞指针"（Bump Pointer）分配策略：
//   1. 预分配一个 64KB 的内存块
//   2. 每次分配只需要移动指针，不需要 free/malloc 系统调用
//   3. 如果当前块空间不足，自动分配更大的新块
//   4. 所有在 Arena 中分配的字符串通过 string_view 引用，无需拷贝
//   5. 整个 Arena 在 DataBatch.Clear() 或生命周期结束时统一释放
//
// 这种设计使得数据在整个流水线中只需要一次字符串内部化（Intern），
// 后续所有阶段都通过 string_view 引用同一个 Arena 中的内存，极大减少了
// 内存分配和拷贝的开销。
//
// 关键方法：
// =========
// - Allocate(size, alignment): 在 Arena 中分配对齐内存
// - CopyString(src): 将字符串拷贝到 Arena 并返回 string_view 引用
// - New<T>(args...): 在 Arena 中构造 C++ 对象（placement new）
// - Reset(): 重置 Arena 状态（保留第一个块，后续块释放）
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string_view>

namespace illuminator {

class Arena {
public:
    static constexpr size_t kDefaultBlockSize = 64 * 1024;  // 默认块大小 64 KB

    // 构造时立即分配第一个内存块
    explicit Arena(size_t block_size = kDefaultBlockSize)
        : block_size_(block_size) {
        AllocateBlock(block_size_);
    }

    // 析构时释放所有块
    ~Arena() {
        for (auto& blk : blocks_) {
            std::free(blk.data);
        }
    }

    // 禁止拷贝：Arena 拥有独占的内存所有权
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 支持移动语义：转移所有权，源对象状态置空
    Arena(Arena&& other) noexcept
        : blocks_(std::move(other.blocks_)),
          current_offset_(other.current_offset_),
          block_size_(other.block_size_),
          total_allocated_(other.total_allocated_) {
        other.current_offset_ = 0;
        other.total_allocated_ = 0;
    }

    // ---- 核心分配方法 ----
    // 在 Arena 中分配指定大小的对齐内存
    // size:      需要分配的字节数
    // alignment: 内存对齐要求（默认 max_align_t 对齐）
    // 返回:      分配的内存指针（来自当前块或新分配的块）
    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        // 计算对齐后的偏移量
        size_t aligned_offset = AlignUp(current_offset_, alignment);
        auto& current = blocks_.back();

        // 如果当前块剩余空间不足，分配新块
        if (aligned_offset + size > current.capacity) {
            // 新块大小取默认块大小和请求大小中的较大值 + 对齐余量
            size_t new_block_size = std::max(block_size_, size + alignment);
            AllocateBlock(new_block_size);
            aligned_offset = AlignUp(0, alignment);  // 新块从头开始对齐
        }

        // 在当前块中分配
        auto& blk = blocks_.back();
        void* ptr = static_cast<uint8_t*>(blk.data) + aligned_offset;
        current_offset_ = aligned_offset + size;  // 更新偏移指针
        total_allocated_ += size;
        return ptr;
    }

    // ---- 字符串内部化（Intern） ----
    // 将外部字符串拷贝到 Arena 中，返回指向 Arena 内部内存的 string_view。
    // 这是实现零拷贝数据流的关键方法。
    // 注意：返回的 string_view 在 Arena 生命周期内有效。
    std::string_view CopyString(std::string_view src) {
        if (src.empty()) return {};
        char* dst = static_cast<char*>(Allocate(src.size()));
        std::memcpy(dst, src.data(), src.size());
        return {dst, src.size()};
    }

    // ---- 对象构造 ----
    // 在 Arena 中通过 placement new 构造 C++ 对象
    // 用法：auto* obj = arena.New<MyClass>(arg1, arg2);
    template <typename T, typename... Args>
    T* New(Args&&... args) {
        void* ptr = Allocate(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // ---- 重置 ----
    // 释放除第一个块外的所有块，重置偏移指针。
    // 这样避免了频繁的 malloc/free，同时快速释放不再需要的内存。
    // 在 DataBatch.Clear() 中调用。
    void Reset() {
        while (blocks_.size() > 1) {
            std::free(blocks_.back().data);
            blocks_.pop_back();
        }
        current_offset_ = 0;
        total_allocated_ = 0;
    }

    // ---- 统计信息 ----
    size_t TotalAllocated() const { return total_allocated_; }
    size_t BlockCount() const { return blocks_.size(); }

private:
    // 内存块描述
    struct Block {
        void* data;       // 块内存起始地址
        size_t capacity;  // 块容量（字节）
    };

    // 计算对齐后的偏移量
    // 示例：offset=17, alignment=8 → 返回 24
    static size_t AlignUp(size_t offset, size_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    // 分配一个新的内存块
    void AllocateBlock(size_t size) {
        void* data = std::malloc(size);
        blocks_.push_back({data, size});
        current_offset_ = 0;  // 新块从头开始分配
    }

    std::vector<Block> blocks_;     // 内存块列表
    size_t current_offset_ = 0;     // 当前块内的分配偏移
    size_t block_size_;             // 默认块大小
    size_t total_allocated_ = 0;    // 已分配总字节数
};

}  // namespace illuminator
