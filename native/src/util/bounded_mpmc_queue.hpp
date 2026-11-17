// Bounded, lock-free, multi-producer / multi-consumer queue.
//
// Design goals (these are engine-wide rules, see docs/ENGINE_DESIGN.md):
//   * Fixed capacity chosen at construction; no allocation afterwards.
//   * try_* operations never block and never make system calls, so they are
//     safe to call from real-time audio/video threads.
//   * A full queue REJECTS the item. The caller decides the overload policy
//     (drop newest, drop oldest, count, signal degraded state...). There is
//     no unbounded fallback.
//
// Algorithm: per-slot sequence numbers (a well-known technique for bounded
// MPMC ring buffers). Each slot's sequence tells producers and consumers
// whether the slot is free for the lap they are on, so the only shared
// read-modify-write is a CAS on the head or tail position.
//
// Items are written and read IN PLACE through callables, which avoids
// copying large slots (e.g. log records, frame descriptors). The callables
// must be noexcept: a throwing writer would leave a slot permanently claimed.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mixit {

inline constexpr std::size_t kCacheLineSize = 64;

template <typename T>
class BoundedMpmcQueue {
    static_assert(std::is_default_constructible_v<T>, "slots are pre-constructed");

public:
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;

    // capacity is rounded up to the next power of two (minimum 2).
    explicit BoundedMpmcQueue(std::size_t capacity) {
        if (capacity == 0 || capacity > kMaxCapacity) {
            throw std::invalid_argument("BoundedMpmcQueue capacity out of range");
        }
        const std::size_t rounded = std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity);
        cells_ = std::make_unique<Cell[]>(rounded);
        mask_ = rounded - 1;
        for (std::size_t i = 0; i < rounded; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    BoundedMpmcQueue(const BoundedMpmcQueue&) = delete;
    BoundedMpmcQueue& operator=(const BoundedMpmcQueue&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // Claims a free slot and calls writer(T&) to fill it in place.
    // Returns false (without calling writer) if the queue is full.
    template <typename Writer>
    bool try_push_with(Writer&& writer) noexcept {
        static_assert(std::is_nothrow_invocable_v<Writer&, T&>, "writer must be noexcept");
        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        writer(cell->value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_push(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        return try_push_with([&value](T& slot) noexcept { slot = value; });
    }

    // Takes the oldest published item and calls reader(T&) on it in place.
    // Returns false (without calling reader) if no item is ready.
    template <typename Reader>
    bool try_pop_with(Reader&& reader) noexcept {
        static_assert(std::is_nothrow_invocable_v<Reader&, T&>, "reader must be noexcept");
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty (or the next producer has not published yet)
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        reader(cell->value);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        return try_pop_with([&out](T& slot) noexcept { out = std::move(slot); });
    }

    // Approximate; only meaningful when producers/consumers are quiescent.
    [[nodiscard]] std::size_t approximate_size() const noexcept {
        const std::size_t head = dequeue_pos_.load(std::memory_order_acquire);
        const std::size_t tail = enqueue_pos_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : 0;
    }

private:
    struct alignas(kCacheLineSize) Cell {
        std::atomic<std::size_t> sequence{0};
        T value{};
    };

    std::unique_ptr<Cell[]> cells_;
    std::size_t mask_ = 0;
    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace mixit
