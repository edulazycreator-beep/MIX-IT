// Tests through the public C ABI only — exactly what the C# host sees.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mixit/mixit_engine.h"

namespace {

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

mixit_engine_config default_config() {
    mixit_engine_config c{};
    c.struct_size = sizeof(c);
    c.api_version = MIXIT_API_VERSION;
    c.min_log_level = MIXIT_LOG_DEBUG;
    return c;
}

struct EngineGuard {
    mixit_engine handle = nullptr;
    ~EngineGuard() {
        if (handle != nullptr) (void)mixit_engine_destroy(handle);
    }
};

}  // namespace

TEST(EngineAbi, VersionAndBuildInfo) {
    const uint32_t v = mixit_get_api_version();
    EXPECT_EQ(v, MIXIT_API_VERSION);
    EXPECT_EQ(MIXIT_VERSION_MAJOR(v), 0u);
    EXPECT_EQ(MIXIT_VERSION_MINOR(v), 3u);
    EXPECT_NE(std::string(mixit_get_build_info()).find("MIX IT native engine 0.3.0"), std::string::npos);
}

TEST(EngineAbi, ResultNames) {
    EXPECT_STREQ(mixit_result_name(MIXIT_OK), "MIXIT_OK");
    EXPECT_STREQ(mixit_result_name(MIXIT_E_INVALID_HANDLE), "MIXIT_E_INVALID_HANDLE");
    EXPECT_STREQ(mixit_result_name(12345), "MIXIT_E_UNKNOWN");
}

TEST(EngineAbi, MonotonicClockAdvances) {
    const uint64_t a = mixit_get_monotonic_time_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const uint64_t b = mixit_get_monotonic_time_ns();
    EXPECT_GE(b - a, 1'000'000u);
}

TEST(EngineAbi, CreateValidatesArguments) {
    mixit_engine e = reinterpret_cast<mixit_engine>(0x1);
    EXPECT_EQ(mixit_engine_create(nullptr, &e), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(e, nullptr) << "out handle must be cleared on failure";
    EXPECT_NE(last_error().find("config is NULL"), std::string::npos);

    mixit_engine_config c = default_config();
    EXPECT_EQ(mixit_engine_create(&c, nullptr), MIXIT_E_INVALID_ARGUMENT);

    c.struct_size = 8;
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_E_INVALID_ARGUMENT);

    c = default_config();
    c.min_log_level = 99;
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_E_INVALID_ARGUMENT);
}

TEST(EngineAbi, CreateRejectsIncompatibleApiVersion) {
    mixit_engine_config c = default_config();
    mixit_engine e = nullptr;
    c.api_version = MIXIT_MAKE_VERSION(1, 0, 0);
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_E_VERSION_MISMATCH);
    c.api_version = MIXIT_MAKE_VERSION(0, 2, 0);
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_E_VERSION_MISMATCH) << "0.x requires an exact minor match";
    c.api_version = MIXIT_MAKE_VERSION(0, 3, 7);
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_OK) << "patch differences are compatible";
    EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK);
}

TEST(EngineAbi, AcceptsLargerStructFromNewerCaller) {
    struct NewerConfig {
        mixit_engine_config v1;
        uint64_t future_field;
    } nc{};
    nc.v1 = default_config();
    nc.v1.struct_size = sizeof(nc);
    EngineGuard g;
    ASSERT_EQ(mixit_engine_create(&nc.v1, &g.handle), MIXIT_OK);

    struct NewerDiag {
        mixit_engine_diagnostics v1;
        uint64_t future_field;
    } nd{};
    nd.v1.struct_size = sizeof(nd);
    nd.future_field = 0xDEADBEEF;
    ASSERT_EQ(mixit_engine_get_diagnostics(g.handle, &nd.v1), MIXIT_OK);
    EXPECT_EQ(nd.v1.struct_size, sizeof(nd)) << "caller's struct_size is preserved";
    EXPECT_EQ(nd.future_field, 0xDEADBEEFu) << "engine must not write past what it knows";
}

TEST(EngineAbi, LifecycleCreateShutdownDestroy) {
    mixit_engine e = nullptr;
    mixit_engine_config c = default_config();
    ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK) << last_error();
    ASSERT_NE(e, nullptr);

    mixit_engine_diagnostics d{};
    d.struct_size = sizeof(d);
    ASSERT_EQ(mixit_engine_get_diagnostics(e, &d), MIXIT_OK);
    EXPECT_EQ(d.state, MIXIT_ENGINE_STATE_INITIALIZED);

    EXPECT_EQ(mixit_engine_shutdown(e), MIXIT_OK);
    EXPECT_EQ(mixit_engine_shutdown(e), MIXIT_OK) << "shutdown is idempotent";
    ASSERT_EQ(mixit_engine_get_diagnostics(e, &d), MIXIT_OK) << "diagnostics remain available after shutdown";
    EXPECT_EQ(d.state, MIXIT_ENGINE_STATE_SHUT_DOWN);
    EXPECT_EQ(d.log_records_delivered, d.log_records_accepted) << "shutdown drains the log";

    mixit_system_capabilities caps{};
    caps.struct_size = sizeof(caps);
    EXPECT_EQ(mixit_engine_detect_capabilities(e, &caps), MIXIT_E_INVALID_STATE);
    EXPECT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "t", "m"), MIXIT_E_INVALID_STATE);

    EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK);
    EXPECT_EQ(mixit_engine_destroy(e), MIXIT_E_INVALID_HANDLE) << "double destroy is detected";
    EXPECT_EQ(mixit_engine_get_diagnostics(e, &d), MIXIT_E_INVALID_HANDLE) << "use after destroy is detected";
}

TEST(EngineAbi, RejectsNullAndForeignHandles) {
    mixit_engine_diagnostics d{};
    d.struct_size = sizeof(d);
    EXPECT_EQ(mixit_engine_get_diagnostics(nullptr, &d), MIXIT_E_INVALID_HANDLE);
    EXPECT_EQ(mixit_engine_get_diagnostics(reinterpret_cast<mixit_engine>(0xBADC0DE), &d), MIXIT_E_INVALID_HANDLE);
    EXPECT_EQ(mixit_engine_shutdown(nullptr), MIXIT_E_INVALID_HANDLE);
    EXPECT_EQ(mixit_engine_destroy(nullptr), MIXIT_E_INVALID_HANDLE);
    EXPECT_NE(last_error().find("mixit_engine_destroy: invalid engine handle"), std::string::npos) << last_error();
}

TEST(EngineAbi, LogCallbackReceivesEngineAndHostRecords) {
    struct Sink {
        std::mutex m;
        std::vector<std::string> lines;
    } sink;
    mixit_engine_config c = default_config();
    c.log_callback = [](const mixit_log_record* r, void* user) {
        auto* s = static_cast<Sink*>(user);
        std::lock_guard lock(s->m);
        s->lines.push_back(std::string(r->category) + "|" + r->message);
    };
    c.log_user_data = &sink;

    mixit_engine e = nullptr;
    ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK);
    ASSERT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "desktop", "hello from host"), MIXIT_OK);
    ASSERT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, nullptr, "no category"), MIXIT_OK);
    EXPECT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "x", nullptr), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_engine_log(e, MIXIT_LOG_OFF, "x", "y"), MIXIT_E_INVALID_ARGUMENT);
    ASSERT_EQ(mixit_engine_destroy(e), MIXIT_OK);  // flushes; no callbacks afterwards

    std::lock_guard lock(sink.m);
    ASSERT_GE(sink.lines.size(), 4u);
    EXPECT_EQ(sink.lines.front().rfind("engine|engine initialized", 0), 0u) << sink.lines.front();
    EXPECT_NE(std::find(sink.lines.begin(), sink.lines.end(), "desktop|hello from host"), sink.lines.end());
    EXPECT_NE(std::find(sink.lines.begin(), sink.lines.end(), "host|no category"), sink.lines.end());
    EXPECT_EQ(sink.lines.back().rfind("engine|engine shutting down", 0), 0u) << sink.lines.back();
}

TEST(EngineAbi, SetLogLevelFiltersAtRuntime) {
    std::atomic<int> count{0};
    mixit_engine_config c = default_config();
    c.min_log_level = MIXIT_LOG_ERROR;
    c.log_callback = [](const mixit_log_record*, void* user) { static_cast<std::atomic<int>*>(user)->fetch_add(1); };
    c.log_user_data = &count;
    mixit_engine e = nullptr;
    ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK);
    ASSERT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "t", "filtered"), MIXIT_OK);
    ASSERT_EQ(mixit_engine_set_log_level(e, MIXIT_LOG_TRACE), MIXIT_OK);
    ASSERT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "t", "kept"), MIXIT_OK);
    EXPECT_EQ(mixit_engine_set_log_level(e, -5), MIXIT_E_INVALID_ARGUMENT);
    ASSERT_EQ(mixit_engine_set_log_level(e, MIXIT_LOG_OFF), MIXIT_OK);  // silence shutdown message
    ASSERT_EQ(mixit_engine_destroy(e), MIXIT_OK);
    EXPECT_EQ(count.load(), 1);
}

// Calling destroy from inside the log callback would join the logging
// thread from itself. The engine must refuse instead of deadlocking.
TEST(EngineAbi, DestroyFromLogCallbackIsRefused) {
    struct Ctx {
        std::atomic<mixit_engine> engine{nullptr};
        std::atomic<mixit_result> result{MIXIT_OK};
        std::atomic<bool> attempted{false};
    } ctx;
    mixit_engine_config c = default_config();
    c.log_callback = [](const mixit_log_record* r, void* user) {
        auto* x = static_cast<Ctx*>(user);
        if (std::strcmp(r->message, "trigger") == 0 && !x->attempted.exchange(true)) {
            x->result = mixit_engine_destroy(x->engine.load());
        }
    };
    c.log_user_data = &ctx;
    mixit_engine e = nullptr;
    ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK);
    ctx.engine = e;
    ASSERT_EQ(mixit_engine_log(e, MIXIT_LOG_INFO, "t", "trigger"), MIXIT_OK);
    for (int i = 0; i < 500 && !ctx.attempted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(ctx.attempted);
    EXPECT_EQ(ctx.result.load(), MIXIT_E_INVALID_STATE);
    EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK) << "the handle is still valid and destroyable from the owner";
}

TEST(EngineAbi, LogFileIsCreated) {
    const std::string path = (std::filesystem::temp_directory_path() / "mixit_engine_abi_test.jsonl").string();
    std::filesystem::remove(path);
    mixit_engine_config c = default_config();
    c.log_file_path = path.c_str();
    mixit_engine e = nullptr;
    ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK) << last_error();

    // Support staff must be able to read the log while the engine is running
    // (on Windows a file opened with _wfopen_s cannot be opened by anyone else).
    std::string live;
    for (int i = 0; i < 200 && live.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ifstream reader(path, std::ios::binary);
        ASSERT_TRUE(reader.is_open()) << "log file is locked while the engine runs";
        std::ostringstream content;
        content << reader.rdbuf();
        live = content.str();
    }
    EXPECT_NE(live.find("engine initialized"), std::string::npos) << live;

    ASSERT_EQ(mixit_engine_destroy(e), MIXIT_OK);
    EXPECT_GT(std::filesystem::file_size(path), 0u);
    std::filesystem::remove(path);

    c.log_file_path = "/no/such/dir/x.jsonl";
    EXPECT_EQ(mixit_engine_create(&c, &e), MIXIT_E_IO);
    EXPECT_NE(last_error().find("cannot open log file"), std::string::npos);
}

TEST(EngineAbi, CapabilityDetectionAndGpuQueries) {
    EngineGuard g;
    mixit_engine_config c = default_config();
    ASSERT_EQ(mixit_engine_create(&c, &g.handle), MIXIT_OK);

    mixit_gpu_adapter_info gpu{};
    gpu.struct_size = sizeof(gpu);
    EXPECT_EQ(mixit_engine_get_gpu_adapter(g.handle, 0, &gpu), MIXIT_E_INVALID_STATE) << "before detection";

    mixit_system_capabilities caps{};
    caps.struct_size = sizeof(caps);
    ASSERT_EQ(mixit_engine_detect_capabilities(g.handle, &caps), MIXIT_OK) << last_error();
    EXPECT_TRUE(caps.detection_flags & MIXIT_DETECTED_CPU);
    EXPECT_TRUE(caps.detection_flags & MIXIT_DETECTED_MEMORY);
    EXPECT_GT(caps.logical_processor_count, 0u);
    EXPECT_GT(caps.total_physical_memory_bytes, 0u);
    EXPECT_LE(caps.available_physical_memory_bytes, caps.total_physical_memory_bytes);
    EXPECT_LT(std::strlen(caps.cpu_brand), sizeof(caps.cpu_brand));

    for (uint32_t i = 0; i < caps.gpu_adapter_count; ++i) {
        ASSERT_EQ(mixit_engine_get_gpu_adapter(g.handle, i, &gpu), MIXIT_OK);
        EXPECT_GT(std::strlen(gpu.description), 0u);
    }
    EXPECT_EQ(mixit_engine_get_gpu_adapter(g.handle, caps.gpu_adapter_count, &gpu), MIXIT_E_NOT_FOUND);

    caps.struct_size = 4;
    EXPECT_EQ(mixit_engine_detect_capabilities(g.handle, &caps), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_engine_detect_capabilities(g.handle, nullptr), MIXIT_E_INVALID_ARGUMENT);
}

// Destroy racing with in-flight calls on other threads: in-flight calls
// finish safely, later calls get MIXIT_E_INVALID_HANDLE, nothing crashes.
TEST(EngineAbi, ConcurrentCallsWhileDestroying) {
    for (int round = 0; round < 20; ++round) {
        mixit_engine_config c = default_config();
        mixit_engine e = nullptr;
        ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK);
        std::atomic<bool> go{false};
        std::atomic<int> unexpected{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&] {
                while (!go) std::this_thread::yield();
                mixit_engine_diagnostics d{};
                d.struct_size = sizeof(d);
                for (int i = 0; i < 2000; ++i) {
                    const mixit_result r1 = mixit_engine_log(e, MIXIT_LOG_DEBUG, "race", "x");
                    const mixit_result r2 = mixit_engine_get_diagnostics(e, &d);
                    const bool ok1 = r1 == MIXIT_OK || r1 == MIXIT_E_INVALID_HANDLE || r1 == MIXIT_E_INVALID_STATE;
                    const bool ok2 = r2 == MIXIT_OK || r2 == MIXIT_E_INVALID_HANDLE;
                    if (!ok1 || !ok2) unexpected.fetch_add(1);
                }
            });
        }
        go = true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK);
        for (auto& w : workers) w.join();
        EXPECT_EQ(unexpected.load(), 0);
    }
}

TEST(EngineAbi, ManyEnginesCanCoexist) {
    std::vector<mixit_engine> engines(8, nullptr);
    mixit_engine_config c = default_config();
    for (auto& e : engines) ASSERT_EQ(mixit_engine_create(&c, &e), MIXIT_OK);
    for (std::size_t i = 0; i < engines.size(); ++i)
        for (std::size_t j = i + 1; j < engines.size(); ++j) EXPECT_NE(engines[i], engines[j]);
    for (auto e : engines) EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK);
}
