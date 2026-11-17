#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logging/logger.hpp"

using namespace mixit;

namespace {

struct Captured {
    int level;
    std::string category;
    std::string message;
    std::uint64_t sequence;
    std::uint64_t thread_id;
};

struct Collector {
    std::mutex mutex;
    std::vector<Captured> records;
    std::atomic<int> callbacks{0};

    static void MIXIT_CALL callback(const mixit_log_record* r, void* user) {
        auto* self = static_cast<Collector*>(user);
        std::lock_guard lock(self->mutex);
        self->records.push_back({r->level, r->category, r->message, r->sequence, r->thread_id});
        self->callbacks.fetch_add(1);
    }
};

std::unique_ptr<Logger> make_logger(Collector& c, std::int32_t level = MIXIT_LOG_TRACE, std::uint32_t capacity = 256,
                                    std::string file = {}) {
    LoggerOptions o;
    o.min_level = level;
    o.queue_capacity = capacity;
    o.callback = &Collector::callback;
    o.user_data = &c;
    o.file_path = std::move(file);
    std::unique_ptr<Logger> logger;
    const Status s = Logger::create(o, logger);
    EXPECT_TRUE(s.is_ok()) << s.message();
    return logger;
}

std::filesystem::path temp_file(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p;
}

}  // namespace

TEST(Logger, DeliversRecordsInOrderOnLoggerThread) {
    Collector c;
    auto logger = make_logger(c);
    for (int i = 0; i < 100; ++i) logger->logf(MIXIT_LOG_INFO, "test", "message {}", i);
    ASSERT_TRUE(logger->flush());
    std::lock_guard lock(c.mutex);
    ASSERT_EQ(c.records.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(c.records[static_cast<std::size_t>(i)].message, "message " + std::to_string(i));
        EXPECT_EQ(c.records[static_cast<std::size_t>(i)].sequence, static_cast<std::uint64_t>(i));
        EXPECT_EQ(c.records[static_cast<std::size_t>(i)].category, "test");
    }
}

TEST(Logger, FiltersBelowMinimumLevelAndCanChangeAtRuntime) {
    Collector c;
    auto logger = make_logger(c, MIXIT_LOG_WARNING);
    EXPECT_FALSE(logger->log(MIXIT_LOG_INFO, "t", "filtered"));
    EXPECT_TRUE(logger->log(MIXIT_LOG_ERROR, "t", "kept"));
    logger->set_min_level(MIXIT_LOG_DEBUG);
    EXPECT_TRUE(logger->log(MIXIT_LOG_DEBUG, "t", "now kept"));
    ASSERT_TRUE(logger->flush());
    std::lock_guard lock(c.mutex);
    ASSERT_EQ(c.records.size(), 2u);
    EXPECT_EQ(c.records[0].message, "kept");
    EXPECT_EQ(c.records[1].message, "now kept");
    EXPECT_EQ(logger->stats().dropped, 0u) << "filtering is not dropping";
}

TEST(Logger, TruncatesLongMessagesWithMarkerOnUtf8Boundary) {
    Collector c;
    auto logger = make_logger(c);
    std::string longmsg;
    while (longmsg.size() < 2000) longmsg += "\xE2\x82\xAC";  // euro sign, 3 bytes
    logger->log(MIXIT_LOG_INFO, "a-category-name-that-is-far-too-long-to-fit", longmsg);
    ASSERT_TRUE(logger->flush());
    std::lock_guard lock(c.mutex);
    ASSERT_EQ(c.records.size(), 1u);
    const std::string& m = c.records[0].message;
    ASSERT_LT(m.size(), LogSlot::kMessageCapacity);
    ASSERT_EQ(m.substr(m.size() - 3), "...");
    EXPECT_EQ((m.size() - 3) % 3, 0u) << "truncated inside a UTF-8 sequence";
    EXPECT_EQ(c.records[0].category.substr(c.records[0].category.size() - 3), "...");
}

// The producer path must never block: when the logger thread is stalled
// (here: a slow callback) and the queue fills, records are dropped and counted.
TEST(Logger, DropsAndCountsWhenQueueIsFull) {
    std::atomic<bool> release{false};
    std::atomic<int> delivered{0};
    struct Ctx { std::atomic<bool>* release; std::atomic<int>* delivered; } ctx{&release, &delivered};
    LoggerOptions o;
    o.min_level = MIXIT_LOG_TRACE;
    o.queue_capacity = 8;
    o.callback = [](const mixit_log_record*, void* user) {
        auto* x = static_cast<Ctx*>(user);
        while (!x->release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        x->delivered->fetch_add(1);
    };
    o.user_data = &ctx;
    std::unique_ptr<Logger> logger;
    ASSERT_TRUE(Logger::create(o, logger).is_ok());

    const auto start = std::chrono::steady_clock::now();
    int accepted = 0;
    for (int i = 0; i < 1000; ++i) accepted += logger->log(MIXIT_LOG_INFO, "t", "x") ? 1 : 0;
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, std::chrono::milliseconds(500)) << "producers must not block on a stalled sink";
    const LoggerStats s = logger->stats();
    EXPECT_EQ(s.accepted + s.dropped, 1000u);
    EXPECT_LE(s.accepted, 9u) << "capacity 8 (+1 possibly in-flight in the callback)";
    EXPECT_GE(s.dropped, 991u);
    EXPECT_EQ(static_cast<std::uint64_t>(accepted), s.accepted);

    release = true;
    logger->stop();
    EXPECT_EQ(static_cast<std::uint64_t>(delivered.load()), s.accepted) << "accepted records are still delivered on stop";
}

TEST(Logger, StopDrainsEverythingAndIsIdempotent) {
    Collector c;
    auto logger = make_logger(c, MIXIT_LOG_TRACE, 4096);
    for (int i = 0; i < 1000; ++i) logger->log(MIXIT_LOG_DEBUG, "t", "m");
    logger->stop();
    logger->stop();
    EXPECT_EQ(c.callbacks.load(), 1000);
    EXPECT_FALSE(logger->log(MIXIT_LOG_ERROR, "t", "after stop"));
    EXPECT_EQ(c.callbacks.load(), 1000) << "no callbacks after stop";
}

TEST(Logger, ConcurrentProducers) {
    Collector c;
    auto logger = make_logger(c, MIXIT_LOG_TRACE, 1 << 16);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < 2000; ++i) logger->logf(MIXIT_LOG_INFO, "mt", "t{} i{}", t, i);
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_TRUE(logger->flush());
    const LoggerStats s = logger->stats();
    EXPECT_EQ(s.accepted + s.dropped, 16000u);
    EXPECT_EQ(s.delivered, s.accepted);
    EXPECT_EQ(static_cast<std::uint64_t>(c.callbacks.load()), s.accepted);
}

TEST(Logger, WritesJsonLinesFile) {
    const auto path = temp_file("mixit_logger_test.jsonl");
    {
        Collector c;
        auto logger = make_logger(c, MIXIT_LOG_TRACE, 64, path.string());
        logger->log(MIXIT_LOG_WARNING, "file", "quote \" and \\ backslash\nnewline");
        logger->log(MIXIT_LOG_INFO, "file", "second");
        logger->stop();
        EXPECT_EQ(logger->stats().file_write_errors, 0u);
    }
    std::vector<std::string> lines;
    {
        std::ifstream in(path);  // closed before remove(): Windows cannot delete open files
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].front(), '{');
    EXPECT_EQ(lines[0].back(), '}');
    EXPECT_NE(lines[0].find("\"level\":\"warning\""), std::string::npos);
    EXPECT_NE(lines[0].find("\"category\":\"file\""), std::string::npos);
    EXPECT_NE(lines[0].find(R"("msg":"quote \" and \\ backslash\nnewline")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("\"ts\":\""), std::string::npos);
    EXPECT_NE(lines[1].find("\"seq\":1"), std::string::npos);
    std::filesystem::remove(path);
}

TEST(Logger, FailsCleanlyOnUnopenableFile) {
    LoggerOptions o;
    o.file_path = "/nonexistent-dir-mixit/xyz/log.jsonl";
#if defined(_WIN32)
    o.file_path = "Z:\\nonexistent-dir-mixit\\xyz\\log.jsonl";
#endif
    std::unique_ptr<Logger> logger;
    const Status s = Logger::create(o, logger);
    EXPECT_EQ(s.code(), MIXIT_E_IO);
    EXPECT_EQ(logger, nullptr);
}

TEST(Logger, RejectsInvalidLevel) {
    LoggerOptions o;
    o.min_level = 42;
    std::unique_ptr<Logger> logger;
    EXPECT_EQ(Logger::create(o, logger).code(), MIXIT_E_INVALID_ARGUMENT);
}
