#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "media/media_queue.hpp"
#include "media/media_types.hpp"

using namespace mixit::media;

TEST(Pool, AcquireReleaseAndExhaustion) {
    Pool<int> pool(3, [](int& v, std::uint32_t i) { v = static_cast<int>(i) * 10; });
    auto a = pool.acquire();
    auto b = pool.acquire();
    auto c = pool.acquire();
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(pool.in_use(), 3u);
    auto d = pool.acquire();
    EXPECT_FALSE(d) << "exhausted pool must not grow";
    EXPECT_EQ(pool.exhausted_count(), 1u);
    b.reset();
    EXPECT_EQ(pool.in_use(), 2u);
    auto e = pool.acquire();
    ASSERT_TRUE(e);
    EXPECT_EQ(*e, 10) << "payload initialized once and reused";
}

TEST(Pool, RefCountingSharesAndReleasesOnce) {
    Pool<int> pool(1, nullptr);
    {
        auto a = pool.acquire();
        auto copy = a;
        auto moved = std::move(copy);
        EXPECT_FALSE(copy);
        EXPECT_EQ(a.use_count(), 2u);
        EXPECT_TRUE(a == moved);
        a.reset();
        EXPECT_EQ(pool.in_use(), 1u) << "still referenced";
    }
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_TRUE(pool.acquire());
}

TEST(Pool, ConcurrentAcquireReleaseKeepsAccounting) {
    Pool<std::uint64_t> pool(16, nullptr);
    std::vector<std::thread> threads;
    std::atomic<std::uint64_t> acquired{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50'000; ++i) {
                auto r = pool.acquire();
                if (r) {
                    ++*r;
                    auto shared = r;  // cross-ref churn
                    acquired.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_GT(acquired.load(), 0u);
}

TEST(VideoFramePool, FramesAreAlignedAndSized) {
    VideoFramePool pool(1918, 1080, 2);
    auto f = pool.acquire();
    ASSERT_TRUE(f);
    EXPECT_EQ(f->width, 1918u);
    EXPECT_EQ(f->stride % 64, 0u);
    EXPECT_GE(f->stride, 1918u * 4);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(f->data) % 64, 0u);
    auto g = pool.acquire();
    EXPECT_GE(static_cast<std::size_t>(g->data - f->data), static_cast<std::size_t>(f->stride) * 1080);
}

TEST(MediaQueue, DropNewestRejectsAndKeepsItemWithCaller) {
    Pool<int> pool(8, nullptr);
    MediaQueue<Pool<int>::Ref> q(2, OverflowPolicy::DropNewest);
    for (int i = 0; i < 2; ++i) {
        auto r = pool.acquire();
        EXPECT_TRUE(q.push(std::move(r)));
    }
    auto extra = pool.acquire();
    EXPECT_FALSE(q.push(std::move(extra)));
    EXPECT_TRUE(extra) << "rejected item stays with the caller";
    extra.reset();
    EXPECT_EQ(q.stats().dropped, 1u);
    EXPECT_EQ(pool.in_use(), 2u);
    q.clear();
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(MediaQueue, DropOldestKeepsTheNewestItems) {
    Pool<int> pool(8, [](int& v, std::uint32_t i) { v = static_cast<int>(i); });
    MediaQueue<Pool<int>::Ref> q(2, OverflowPolicy::DropOldest);
    std::vector<int> pushed;
    for (int i = 0; i < 5; ++i) {
        auto r = pool.acquire();
        pushed.push_back(*r);
        EXPECT_TRUE(q.push(std::move(r)));
    }
    const MediaQueueStats s = q.stats();
    EXPECT_EQ(s.pushed, 5u);
    EXPECT_EQ(s.dropped, 3u);
    EXPECT_EQ(s.high_water, 2u);
    EXPECT_EQ(pool.in_use(), 2u) << "dropped items were released to the pool";
    Pool<int>::Ref out;
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(*out, pushed[3]);
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(*out, pushed[4]);
    EXPECT_FALSE(q.pop(out));
}
