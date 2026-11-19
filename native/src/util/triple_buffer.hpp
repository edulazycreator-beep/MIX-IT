// Lock-free single-writer / single-reader "latest value" hand-off.
//
// The writer fills its private slot and swaps it with the shared middle slot;
// the reader swaps the middle slot into its private slot when a new value was
// published. Every slot is owned by exactly one side at a time and ownership
// moves through one atomic exchange (acq_rel), so there is no data race and
// neither side ever waits. Used for real-time parameter and meter exchange.
//
// Writers on different threads must be serialized externally (e.g. the
// session control mutex); the same holds for readers.
#pragma once

#include <array>
#include <atomic>

namespace mixit {

template <typename T>
class TripleBuffer {
public:
    explicit TripleBuffer(const T& initial = T{}) : slots_{initial, initial, initial} {}
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    void publish(const T& value) noexcept {
        slots_[write_] = value;
        write_ = middle_.exchange(write_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }

    // Latest published value (or the previous one if nothing new arrived).
    const T& read() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kFresh) != 0) {
            read_ = middle_.exchange(read_, std::memory_order_acq_rel) & kIndex;
        }
        return slots_[read_];
    }

private:
    static constexpr int kIndex = 3;
    static constexpr int kFresh = 4;
    std::array<T, 3> slots_;
    int write_ = 0;  // writer-owned
    int read_ = 1;   // reader-owned
    std::atomic<int> middle_{2};
};

}  // namespace mixit
