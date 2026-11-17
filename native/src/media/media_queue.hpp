// Bounded media queue with an explicit overflow policy and counters.
// Built on BoundedMpmcQueue: never blocks, never allocates after
// construction. See docs/ENGINE_DESIGN.md §4 for which stage uses which policy.
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "util/bounded_mpmc_queue.hpp"

namespace mixit::media {

enum class OverflowPolicy {
    DropNewest,  // reject the incoming item (keeps continuity, e.g. audio)
    DropOldest,  // discard the oldest queued item (keeps latency low, e.g. video preview)
};

struct MediaQueueStats {
    std::uint64_t pushed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t popped = 0;
    std::uint64_t high_water = 0;
};

template <typename T>
class MediaQueue {
public:
    MediaQueue(std::size_t capacity, OverflowPolicy policy) : queue_(capacity), policy_(policy) {}

    // Returns true if `item` was enqueued. On DropNewest overflow the item
    // is left in `item` (the caller's Ref releases it).
    bool push(T&& item) noexcept {
        if (try_enqueue(item)) return true;
        if (policy_ == OverflowPolicy::DropOldest) {
            T oldest{};
            if (queue_.try_pop(oldest)) dropped_.fetch_add(1, std::memory_order_relaxed);
            // `oldest` released here; retry once (a racing producer may have taken the slot)
            if (try_enqueue(item)) return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool pop(T& out) noexcept {
        if (!queue_.try_pop(out)) return false;
        popped_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void clear() noexcept {
        T discard{};
        while (queue_.try_pop(discard)) discard = T{};
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return queue_.capacity(); }
    [[nodiscard]] std::size_t approximate_size() const noexcept { return queue_.approximate_size(); }

    [[nodiscard]] MediaQueueStats stats() const noexcept {
        return {pushed_.load(std::memory_order_relaxed), dropped_.load(std::memory_order_relaxed),
                popped_.load(std::memory_order_relaxed), high_water_.load(std::memory_order_relaxed)};
    }

private:
    bool try_enqueue(T& item) noexcept {
        if (!queue_.try_push_with([&item](T& slot) noexcept { slot = std::move(item); })) return false;
        pushed_.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t size = queue_.approximate_size();
        std::uint64_t hw = high_water_.load(std::memory_order_relaxed);
        while (size > hw && !high_water_.compare_exchange_weak(hw, size, std::memory_order_relaxed)) {
        }
        return true;
    }

    BoundedMpmcQueue<T> queue_;
    OverflowPolicy policy_;
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> popped_{0};
    std::atomic<std::uint64_t> high_water_{0};
};

}  // namespace mixit::media
