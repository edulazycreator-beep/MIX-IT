#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "util/bounded_mpmc_queue.hpp"

using mixit::BoundedMpmcQueue;

TEST(BoundedMpmcQueue, CapacityIsRoundedUpToPowerOfTwo) {
    EXPECT_EQ(BoundedMpmcQueue<int>(1).capacity(), 2u);
    EXPECT_EQ(BoundedMpmcQueue<int>(3).capacity(), 4u);
    EXPECT_EQ(BoundedMpmcQueue<int>(1000).capacity(), 1024u);
    EXPECT_EQ(BoundedMpmcQueue<int>(4096).capacity(), 4096u);
}

TEST(BoundedMpmcQueue, RejectsInvalidCapacity) {
    EXPECT_THROW(BoundedMpmcQueue<int>(0), std::invalid_argument);
    EXPECT_THROW(BoundedMpmcQueue<int>(BoundedMpmcQueue<int>::kMaxCapacity + 1), std::invalid_argument);
}

TEST(BoundedMpmcQueue, IsFifoAndRejectsWhenFull) {
    BoundedMpmcQueue<int> q(4);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99)) << "a full queue must reject, never grow or block";
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(BoundedMpmcQueue, WrapsAroundManyLaps) {
    BoundedMpmcQueue<std::uint64_t> q(8);
    std::uint64_t out = 0;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_pop(out));
        ASSERT_EQ(out, i);
    }
    EXPECT_EQ(q.approximate_size(), 0u);
}

TEST(BoundedMpmcQueue, InPlaceWriterAndReader) {
    struct Big { char data[256]; };
    BoundedMpmcQueue<Big> q(2);
    ASSERT_TRUE(q.try_push_with([](Big& b) noexcept { b.data[0] = 'x'; b.data[255] = 'y'; }));
    char first = 0, last = 0;
    ASSERT_TRUE(q.try_pop_with([&](Big& b) noexcept { first = b.data[0]; last = b.data[255]; }));
    EXPECT_EQ(first, 'x');
    EXPECT_EQ(last, 'y');
}

// Every pushed value must be popped exactly once across concurrent
// producers and consumers.
TEST(BoundedMpmcQueue, ConcurrentProducersAndConsumersLoseNothing) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr std::uint64_t kPerProducer = 50'000;
    BoundedMpmcQueue<std::uint64_t> q(64);

    std::vector<std::atomic<std::uint8_t>> seen(kProducers * kPerProducer);
    std::atomic<std::uint64_t> popped{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const std::uint64_t value = static_cast<std::uint64_t>(p) * kPerProducer + i;
                while (!q.try_push(value)) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            std::uint64_t v = 0;
            for (;;) {
                if (q.try_pop(v)) {
                    seen[v].fetch_add(1, std::memory_order_relaxed);
                    popped.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load() && q.approximate_size() == 0) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int p = 0; p < kProducers; ++p) threads[static_cast<std::size_t>(p)].join();
    producers_done = true;
    for (std::size_t t = kProducers; t < threads.size(); ++t) threads[t].join();

    EXPECT_EQ(popped.load(), kProducers * kPerProducer);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(seen[i].load(), 1u) << "value " << i;
    }
}
