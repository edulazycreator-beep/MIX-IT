// Minimal VST3 effect used to test the MIX IT host end to end (any platform).
// Written directly on the VST3 interface headers (MIT), single component
// (processor and edit controller in one object).
//
// Parameters: 1 "Gain" (normalized 0..1 → linear 0..2, default 0.5 = unity),
// 2 "Bypass" (kIsBypass). State: the two values as little-endian doubles.
// Audio: out = in × gain (or in, bypassed); mono or stereo.
#include <atomic>
#include <cstdio>
#include <cstring>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// Class id: "MIXITTESTGAIN000" as bytes.
const TUID kGainCid = INLINE_UID(0x4D495849, 0x54544553, 0x54474149, 0x4E303030);
constexpr ParamID kGain = 1;
constexpr ParamID kBypass = 2;

void put_string(String128 dst, const char* src) {
    std::size_t i = 0;
    for (; src[i] != 0 && i < 127; ++i) dst[i] = static_cast<char16>(src[i]);
    dst[i] = 0;
}

class GainPlugin final : public IComponent, public IAudioProcessor, public IEditController {
public:
    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(wanted, IComponent::iid)) {
            *obj = static_cast<IComponent*>(this);
        } else if (FUnknownPrivate::iidEqual(wanted, IAudioProcessor::iid)) {
            *obj = static_cast<IAudioProcessor*>(this);
        } else if (FUnknownPrivate::iidEqual(wanted, IEditController::iid)) {
            *obj = static_cast<IEditController*>(this);
        } else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase (shared by component and controller)
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }  // single component
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = channels_;
        put_string(bus.name, dir == kInput ? "In" : "Out");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream* s) override {
        double v[2];
        int32 got = 0;
        if (s == nullptr || s->read(v, sizeof(v), &got) != kResultOk || got != static_cast<int32>(sizeof(v))) return kResultFalse;
        gain_.store(v[0]);
        bypass_.store(v[1]);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) override {
        double v[2] = {gain_.load(), bypass_.load()};
        return s != nullptr ? s->write(v, sizeof(v), nullptr) : kInvalidArgument;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 nin, SpeakerArrangement* out, int32 nout) override {
        if (nin != 1 || nout != 1 || in[0] != out[0]) return kResultFalse;
        if (in[0] == SpeakerArr::kStereo) {
            channels_ = 2;
        } else if (in[0] == SpeakerArr::kMono) {
            channels_ = 1;
        } else {
            return kResultFalse;
        }
        return kResultTrue;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = channels_ == 2 ? SpeakerArr::kStereo : SpeakerArr::kMono;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.inputParameterChanges != nullptr) {
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
                int32 offset = 0;
                ParamValue v = 0;
                if (q == nullptr || q->getPointCount() == 0 || q->getPoint(q->getPointCount() - 1, offset, v) != kResultOk) continue;
                if (q->getParameterId() == kGain) gain_.store(v);
                if (q->getParameterId() == kBypass) bypass_.store(v);
            }
        }
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        const float g = bypass_.load() >= 0.5 ? 1.0f : static_cast<float>(gain_.load() * 2.0);
        for (int32 c = 0; c < data.outputs[0].numChannels; ++c) {
            const float* in = data.inputs[0].channelBuffers32[c];
            float* out = data.outputs[0].channelBuffers32[c];
            for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] * g;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream* s) override {
        const tresult r = setState(s);
        if (r == kResultOk) {
            controller_gain_ = gain_.load();
            controller_bypass_ = bypass_.load();
        }
        return r;
    }
    // setState/getState: the IComponent overrides above also serve the
    // controller (same signatures; this plug-in has one shared state).
    int32 PLUGIN_API getParameterCount() override { return 2; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index < 0 || index > 1) return kInvalidArgument;
        std::memset(&info, 0, sizeof(info));
        info.id = index == 0 ? kGain : kBypass;
        put_string(info.title, index == 0 ? "Gain" : "Bypass");
        put_string(info.shortTitle, index == 0 ? "Gain" : "Byp");
        put_string(info.units, index == 0 ? "x" : "");
        info.stepCount = index == 0 ? 0 : 1;
        info.defaultNormalizedValue = index == 0 ? 0.5 : 0.0;
        info.flags = ParameterInfo::kCanAutomate | (index == 1 ? ParameterInfo::kIsBypass : 0);
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 out) override {
        char buf[32];
        if (id == kGain) {
            std::snprintf(buf, sizeof(buf), "%.2f", v * 2.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%s", v >= 0.5 ? "on" : "off");
        }
        put_string(out, buf);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override { return id == kGain ? v * 2.0 : v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) override { return id == kGain ? v / 2.0 : v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return id == kGain ? controller_gain_ : id == kBypass ? controller_bypass_ : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id == kGain) controller_gain_ = v;
        else if (id == kBypass) controller_bypass_ = v;
        else return kInvalidArgument;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    std::atomic<uint32> refs_{1};
    int32 channels_ = 2;
    std::atomic<double> gain_{0.5};
    std::atomic<double> bypass_{0.0};
    double controller_gain_ = 0.5;
    double controller_bypass_ = 0.0;
};

class Factory final : public IPluginFactory {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, FUnknown::iid) || FUnknownPrivate::iidEqual(wanted, IPluginFactory::iid)) {
            *obj = static_cast<IPluginFactory*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        std::memset(info, 0, sizeof(*info));
        std::strcpy(info->vendor, "MIX IT tests");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, kGainCid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "MIX IT Test Gain");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (std::memcmp(cid, kGainCid, sizeof(TUID)) != 0) {
            *obj = nullptr;
            return kInvalidArgument;
        }
        auto* p = new GainPlugin();
        const tresult r = p->queryInterface(iid, obj);
        p->release();  // the query added its own reference
        return r;
    }
};

Factory g_factory;

}  // namespace

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return &g_factory; }
#if defined(_WIN32)
SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif
}
