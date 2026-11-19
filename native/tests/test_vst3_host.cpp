// VST3 host against the test plug-in bundle built with the tests.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "audio/vst3_host.hpp"

using namespace mixit::audio;

#if defined(MIXIT_TEST_VST3_BUNDLE)

namespace {
// The class id as scanned. Its bytes differ per platform: on Windows, VST3
// lays out INLINE_UID in COM GUID order.
std::string test_cid() {
    std::vector<Vst3PluginInfo> found;
    std::vector<std::string> errors;
    if (!vst3_scan(MIXIT_TEST_VST3_BUNDLE, found, errors).is_ok() || found.empty()) return {};
    return found[0].class_id;
}
}

TEST(Vst3Host, ScansTheTestBundle) {
    std::vector<Vst3PluginInfo> found;
    std::vector<std::string> errors;
    ASSERT_TRUE(vst3_scan(MIXIT_TEST_VST3_BUNDLE, found, errors).is_ok());
    EXPECT_TRUE(errors.empty()) << errors.front();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].name, "MIX IT Test Gain");
    EXPECT_EQ(found[0].vendor, "MIX IT tests");
    EXPECT_EQ(found[0].class_id.size(), 32u);
#if defined(_WIN32)
    EXPECT_EQ(found[0].class_id, "4958494D54545345544741494E303030") << "COM GUID byte order";
#else
    EXPECT_EQ(found[0].class_id, "4D49584954544553544741494E303030");
#endif
    // Scanning the parent folder finds it too.
    const std::string folder = std::string(MIXIT_TEST_VST3_BUNDLE).substr(0, std::string(MIXIT_TEST_VST3_BUNDLE).rfind('/'));
    ASSERT_TRUE(vst3_scan(folder, found, errors).is_ok());
    EXPECT_EQ(found.size(), 1u);
    EXPECT_EQ(vst3_scan("/no/such/plugin.vst3", found, errors).code(), MIXIT_E_NOT_FOUND);
}

TEST(Vst3Host, ProcessesWithParametersBypassAndState) {
    const std::string kCid = test_cid();
    std::unique_ptr<Vst3Effect> fx;
    const mixit::Status st = Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, kCid, 48000, 2, 480, fx);
    ASSERT_TRUE(st.is_ok()) << st.message();
    ASSERT_EQ(fx->parameters().size(), 2u);
    EXPECT_EQ(fx->parameters()[0].title, "Gain");
    EXPECT_TRUE(fx->parameters()[1].is_bypass);
    EXPECT_EQ(fx->info().name, "MIX IT Test Gain");

    std::vector<float> block(480 * 2, 0.25f);
    fx->process(block.data(), 480);
    EXPECT_FLOAT_EQ(block[0], 0.25f) << "default gain is unity";

    ASSERT_TRUE(fx->set_parameter(1, 1.0).is_ok());  // ×2
    std::fill(block.begin(), block.end(), 0.25f);
    fx->process(block.data(), 480);
    EXPECT_FLOAT_EQ(block[100], 0.5f);
    double v = 0;
    ASSERT_TRUE(fx->get_parameter(1, v).is_ok());
    EXPECT_DOUBLE_EQ(v, 1.0);
    EXPECT_EQ(fx->parameter_text(1, 0.25), "0.50");

    fx->set_bypass(true);  // host bypass: untouched
    std::fill(block.begin(), block.end(), 0.25f);
    fx->process(block.data(), 480);
    EXPECT_FLOAT_EQ(block[7], 0.25f);
    fx->set_bypass(false);

    std::vector<std::uint8_t> state;
    ASSERT_TRUE(fx->get_state(state).is_ok());
    std::unique_ptr<Vst3Effect> other;
    ASSERT_TRUE(Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, kCid, 48000, 1, 480, other).is_ok()) << "mono works too";
    ASSERT_TRUE(other->set_state(state.data(), state.size()).is_ok());
    std::vector<float> mono(480, 0.1f);
    other->process(mono.data(), 480);
    EXPECT_NEAR(mono[0], 0.2f, 1e-6) << "gain ×2 restored from state";

    EXPECT_EQ(fx->set_parameter(1, 1.5).code(), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(fx->set_parameter(99, 0.5).code(), MIXIT_E_NOT_FOUND);
    EXPECT_EQ(other->set_state(state.data(), 5).code(), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, "00", 48000, 2, 480, other).code(), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, "00000000000000000000000000000000", 48000, 2, 480, other).code(),
              MIXIT_E_NOT_FOUND);
    EXPECT_EQ(fx->process_errors(), 0u);
}

#else

TEST(Vst3Host, NotBuilt) {
    std::vector<Vst3PluginInfo> found;
    std::vector<std::string> errors;
    EXPECT_EQ(vst3_scan(".", found, errors).code(), MIXIT_E_NOT_SUPPORTED);
}

#endif

#if defined(MIXIT_TEST_VST3_BUNDLE)
TEST(Vst3Host, ZeroFrameProcessDeliversParametersForState) {
    std::unique_ptr<Vst3Effect> fx;
    ASSERT_TRUE(Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, test_cid(), 48000, 2, 480, fx).is_ok());
    ASSERT_TRUE(fx->set_parameter(1, 0.75).is_ok());
    fx->process(nullptr, 0);  // no audio thread: flush the change to the processor
    std::vector<std::uint8_t> state;
    ASSERT_TRUE(fx->get_state(state).is_ok());
    std::unique_ptr<Vst3Effect> other;
    ASSERT_TRUE(Vst3Effect::create(MIXIT_TEST_VST3_BUNDLE, test_cid(), 48000, 2, 480, other).is_ok());
    ASSERT_TRUE(other->set_state(state.data(), state.size()).is_ok());
    double v = 0;
    ASSERT_TRUE(other->get_parameter(1, v).is_ok());
    EXPECT_DOUBLE_EQ(v, 0.75) << "controller follows the restored component state";
    EXPECT_EQ(other->parameter_text(1, v), "1.50");
}
#endif
