#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string_view>

namespace illuminator {

// Memory arena for zero-copy data pipeline.
// Allocations are bump-pointer; deallocation happens in bulk when the arena is reset.
// Inspired by LoongCollector's SourceBuffer design.
class Arena {
public:
    static constexpr size_t kDefaultBlockSize = 64 * 1024;  // 64 KB

    explicit Arena(size_t block_size = kDefaultBlockSize)
        : block_size_(block_size) {
        AllocateBlock(block_size_);
    }

    ~Arena() {
        for (auto& blk : blocks_) {
            std::free(blk.data);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
        : blocks_(std::move(other.blocks_)),
          current_offset_(other.current_offset_),
          block_size_(other.block_size_),
          total_allocated_(other.total_allocated_) {
        other.current_offset_ = 0;
        other.total_allocated_ = 0;
    }

    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t aligned_offset = AlignUp(current_offset_, alignment);
        auto& current = blocks_.back();

        if (aligned_offset + size > current.capacity) {
            size_t new_block_size = std::max(block_size_, size + alignment);
            AllocateBlock(new_block_size);
            aligned_offset = AlignUp(0, alignment);
        }

        auto& blk = blocks_.back();
        void* ptr = static_cast<uint8_t*>(blk.data) + aligned_offset;
        current_offset_ = aligned_offset + size;
        total_allocated_ += size;
        return ptr;
    }

    // Allocate and copy a string, returning a view into arena memory
    std::string_view CopyString(std::string_view src) {
        if (src.empty()) return {};
        char* dst = static_cast<char*>(Allocate(src.size()));
        std::memcpy(dst, src.data(), src.size());
        return {dst, src.size()};
    }

    template <typename T, typename... Args>
    T* New(Args&&... args) {
        void* ptr = Allocate(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void Reset() {
        while (blocks_.size() > 1) {
            std::free(blocks_.back().data);
            blocks_.pop_back();
        }
        current_offset_ = 0;
        total_allocated_ = 0;
    }

    size_t TotalAllocated() const { return total_allocated_; }
    size_t BlockCount() const { return blocks_.size(); }

private:
    struct Block {
        void* data;
        size_t capacity;
    };

    static size_t AlignUp(size_t offset, size_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    void AllocateBlock(size_t size) {
        void* data = std::malloc(size);
        blocks_.push_back({data, size});
        current_offset_ = 0;
    }

    std::vector<Block> blocks_;
    size_t current_offset_ = 0;
    size_t block_size_;
    size_t total_allocated_ = 0;
};

}  // namespace illuminator
