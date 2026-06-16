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
#include <functional>
#include <memory>
#include <vector>
#include <string_view>

namespace illuminator {

class Arena {
public:
    static constexpr size_t kDefaultBlockSize = 64 * 1024;  // 默认块大小 64 KB

    // 构造时立即分配第一个内存块
    // 这样首次调用 Allocate 时无需再检查 blocks_ 是否为空
    explicit Arena(size_t block_size = kDefaultBlockSize)
        : block_size_(block_size) {
        AllocateBlock(block_size_);
    }

    // 析构时释放所有块：每个块都是通过 std::malloc 分配的，必须用 std::free 逐个释放
    ~Arena() {
        for (auto& blk : blocks_) {
            std::free(blk.data);
        }
    }

    // 禁止拷贝：Arena 拥有独占的内存所有权，两个 Arena 不能共享同一块内存
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 移动构造：将源 Arena 的内存块所有权转移到新对象
    // 源对象的 current_offset_ 和 total_allocated_ 置零，防止析构时 double-free
    // blocks_ 被 move 后源对象变为空 vector，析构安全
    Arena(Arena&& other) noexcept
        : blocks_(std::move(other.blocks_)),
          current_offset_(other.current_offset_),
          block_size_(other.block_size_),
          total_allocated_(other.total_allocated_) {
        other.current_offset_ = 0;
        other.total_allocated_ = 0;
    }

    // ---- 核心分配方法（碰撞指针 / Bump Pointer） ----
    // 碰撞指针的"碰撞"指的就是 current_offset_ 这个指针不断向前"撞"。
    // 每次分配只需：1) 对齐指针  2) 检查空间  3) 指针前移 — 三步完成，无需 malloc。
    //
    // size:      需要分配的字节数
    // alignment: 内存对齐要求（默认 max_align_t 对齐，即平台最大标量类型对齐）
    // 返回:      分配的内存指针（来自当前块或新分配的块）
    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        // 计算对齐后的偏移量。
        // 例如：current_offset_=17, alignment=8 → aligned_offset=24
        size_t aligned_offset = AlignUp(current_offset_, alignment);
        auto& current = blocks_.back();

        // 当前块剩余空间不足 → 分配新块
        if (aligned_offset + size > current.capacity) {
            // 新块大小取默认块大小和本次请求所需空间中的较大值，确保能容纳本次分配
            size_t new_block_size = std::max(block_size_, size + alignment);
            AllocateBlock(new_block_size);
            aligned_offset = AlignUp(0, alignment);  // 新块从头开始对齐
        }

        // 在当前块的 aligned_offset 处"切"出 size 字节
        auto& blk = blocks_.back();
        void* ptr = static_cast<uint8_t*>(blk.data) + aligned_offset;
        current_offset_ = aligned_offset + size;  // 碰撞指针前移
        total_allocated_ += size;
        return ptr;
    }

    // ---- 字符串内部化（Intern） ----
    // 将外部字符串拷贝到 Arena 中，返回指向 Arena 内部内存的 string_view。
    // 这是实现零拷贝数据流的关键方法：字符串只拷贝一次，后续全部用 string_view 引用。
    //
    // 生命周期：返回的 string_view 在 Arena 有效期内一直有效。
    //   由于 DataBatch 通过 shared_ptr<Arena> 共享 Arena 所有权，
    //   只要 DataBatch 还活着，这些 string_view 就不会悬空。
    std::string_view CopyString(std::string_view src) {
        if (src.empty()) return {};
        char* dst = static_cast<char*>(Allocate(src.size()));
        std::memcpy(dst, src.data(), src.size());
        return {dst, src.size()};  // 构造 string_view 指向 Arena 中的副本
    }

    // ---- 对象构造（Placement New） ----
    // 在 Arena 中通过 placement new 构造 C++ 对象。
    // 对象的内存来自 Arena（碰撞指针分配），构造函数的参数完美转发。
    //
    // 用法：auto* obj = arena.New<MyClass>(arg1, arg2);
    //
    // 注意：Arena 不会调用对象的析构函数（只释放原始内存），
    //   因此只适合构造"平凡析构"的对象，或生命周期由 Arena 统一管理的对象。
    template <typename T, typename... Args>
    T* New(Args&&... args) {
        void* ptr = Allocate(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // ---- 重置 ----
    // 释放除第一个块外的所有块，将偏移指针归零。
    // 保留第一个块避免下次使用时重新 malloc，同时释放多余块防止内存膨胀。
    // 在 DataBatch.Clear() 中调用，实现"一次分配、批次内复用"。
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
        void* data;       // 块内存起始地址（malloc 返回的原始指针）
        size_t capacity;  // 块容量（字节）
    };

    // 向上对齐计算：将 offset 对齐到 alignment 的整数倍。
    // 示例：offset=17, alignment=8 → 返回 24
    // 原理： (17 + 8 - 1) & ~(8 - 1) = 24 & ~7 = 24 & 0xFFF...F8 = 24
    static size_t AlignUp(size_t offset, size_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    // 分配一个新内存块，使用 std::malloc 向系统申请原始内存。
    // 分配失败时先尝试 OOM 回调（如通知监控系统），再次失败则 abort。
    void AllocateBlock(size_t size) {
        void* data = std::malloc(size);
        if (!data) {
            if (oom_handler_) {
                oom_handler_(size);  // 通知外部，给一个释放缓存的机会
                data = std::malloc(size);  // 重试
            }
            if (!data) {
                std::abort();  // 彻底失败，无法恢复
            }
        }
        blocks_.push_back({data, size});
        current_offset_ = 0;  // 新块偏移从 0 开始
    }

public:
    // OOM（Out-Of-Memory）回调类型：当 malloc 失败时调用的处理函数
    using OomHandler = std::function<void(size_t requested_bytes)>;

    void SetOomHandler(OomHandler handler) {
        oom_handler_ = std::move(handler);
    }

private:
    OomHandler oom_handler_;          // 内存不足回调（可选，用于监控和告警）

    std::vector<Block> blocks_;       // 已分配的内存块列表
    size_t current_offset_ = 0;       // 当前块内的"碰撞指针"偏移（已经分配到的位置）
    size_t block_size_;               // 新块的默认大小（默认 64KB）
    size_t total_allocated_ = 0;      // 累计分配字节数（统计用）
};

}  // namespace illuminator
