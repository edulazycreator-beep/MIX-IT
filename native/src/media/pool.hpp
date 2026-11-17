// Fixed-capacity object pool with intrusive reference-counted handles.
//
// * All payloads are constructed up front; acquire()/release never allocate,
//   lock or make syscalls, so they are safe on media threads.
// * acquire() returns an empty Ref when the pool is exhausted (counted):
//   the caller applies its overload policy instead of growing memory.
// * Lifetime rule: a Pool must outlive every Ref it handed out. Sessions
//   guarantee this by stopping threads and clearing queues before pools are
//   destroyed; in_use() lets tests verify that nothing leaked.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "util/bounded_mpmc_queue.hpp"

namespace mixit::media {

template <typename Payload>
class Pool {
    struct Slot {
        Payload payload{};
        std::atomic<std::uint32_t> refs{0};
        std::uint32_t index = 0;
        Pool* owner = nullptr;
    };

public:
    class Ref {
    public:
        Ref() noexcept = default;
        Ref(const Ref& other) noexcept : slot_(other.slot_) {
            if (slot_ != nullptr) slot_->refs.fetch_add(1, std::memory_order_relaxed);
        }
        Ref(Ref&& other) noexcept : slot_(std::exchange(other.slot_, nullptr)) {}
        Ref& operator=(const Ref& other) noexcept {
            if (this != &other) {
                Ref copy(other);
                std::swap(slot_, copy.slot_);
            }
            return *this;
        }
        Ref& operator=(Ref&& other) noexcept {
            if (this != &other) {
                reset();
                slot_ = std::exchange(other.slot_, nullptr);
            }
            return *this;
        }
        ~Ref() { reset(); }

        void reset() noexcept {
            if (slot_ == nullptr) return;
            if (slot_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                slot_->owner->release_slot(slot_->index);
            }
            slot_ = nullptr;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }
        Payload* operator->() const noexcept { return &slot_->payload; }
        Payload& operator*() const noexcept { return slot_->payload; }
        [[nodiscard]] std::uint32_t use_count() const noexcept {
            return slot_ == nullptr ? 0 : slot_->refs.load(std::memory_order_relaxed);
        }
        friend bool operator==(const Ref& a, const Ref& b) noexcept { return a.slot_ == b.slot_; }

    private:
        friend class Pool;
        explicit Ref(Slot* slot) noexcept : slot_(slot) {}
        Slot* slot_ = nullptr;
    };

    // `init` is called once per payload at construction (e.g. to attach
    // pre-allocated pixel memory).
    Pool(std::uint32_t capacity, const std::function<void(Payload&, std::uint32_t index)>& init)
        : slots_(std::make_unique<Slot[]>(capacity)), free_(capacity), capacity_(capacity) {
        for (std::uint32_t i = 0; i < capacity; ++i) {
            slots_[i].index = i;
            slots_[i].owner = this;
            if (init) init(slots_[i].payload, i);
            (void)free_.try_push(i);
        }
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    Ref acquire() noexcept {
        std::uint32_t index = 0;
        if (!free_.try_pop(index)) {
            exhausted_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        Slot& slot = slots_[index];
        slot.refs.store(1, std::memory_order_relaxed);
        in_use_.fetch_add(1, std::memory_order_relaxed);
        return Ref(&slot);
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t in_use() const noexcept { return in_use_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t exhausted_count() const noexcept { return exhausted_.load(std::memory_order_relaxed); }

private:
    void release_slot(std::uint32_t index) noexcept {
        in_use_.fetch_sub(1, std::memory_order_relaxed);
        (void)free_.try_push(index);  // cannot fail: capacity == slot count
    }

    std::unique_ptr<Slot[]> slots_;
    BoundedMpmcQueue<std::uint32_t> free_;
    std::uint32_t capacity_;
    std::atomic<std::uint32_t> in_use_{0};
    std::atomic<std::uint64_t> exhausted_{0};
};

}  // namespace mixit::media
