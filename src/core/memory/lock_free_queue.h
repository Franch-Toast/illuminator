#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <new>
#include <type_traits>

namespace illuminator {

// Lock-free MPSC (multiple-producer, single-consumer) bounded queue.
// Inspired by LoongCollector's high/low watermark queue design.
// Uses a fixed-size ring buffer with atomic head/tail pointers.
template <typename T, size_t Capacity = 4096>
class LockFreeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    LockFreeQueue() : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LockFreeQueue() {
        // Drain remaining elements
        T tmp;
        while (TryPop(tmp)) {}
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    // Try to push an element. Returns false if queue is full.
    bool TryPush(const T& value) {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue full
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool TryPush(T&& value) {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Try to pop an element. Single-consumer only.
    bool TryPop(T& value) {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue empty
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        value = std::move(cell->data);
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    size_t SizeApprox() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h >= t ? h - t : 0;
    }

    bool Empty() const { return SizeApprox() == 0; }

    static constexpr size_t capacity() { return Capacity; }

    // Watermark support for backpressure
    bool AboveHighWatermark(double ratio = 0.8) const {
        return SizeApprox() > static_cast<size_t>(Capacity * ratio);
    }

    bool BelowLowWatermark(double ratio = 0.2) const {
        return SizeApprox() < static_cast<size_t>(Capacity * ratio);
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    Cell cells_[Capacity];
};

}  // namespace illuminator
