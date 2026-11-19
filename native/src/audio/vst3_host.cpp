#include "audio/vst3_host.hpp"

#if defined(MIXIT_HAVE_VST3) && MIXIT_HAVE_VST3

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// VST3 interfaces are COM-style: objects are destroyed through release(), so
// the interface classes deliberately have no virtual destructor.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

namespace mixit::audio {

namespace fs = std::filesystem;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ---- helpers --------------------------------------------------------------------

std::string to_utf8(const char16* s, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < max && s[i] != 0; ++i) {
        std::uint32_t c = static_cast<std::uint16_t>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint16_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string bounded(const char8* s, std::size_t max) { return std::string(s, strnlen(s, max)); }

std::string cid_to_hex(const TUID cid) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        const auto b = static_cast<std::uint8_t>(cid[i]);
        s += kHex[b >> 4];
        s += kHex[b & 15];
    }
    return s;
}

bool hex_to_cid(const std::string& hex, TUID out) {
    if (hex.size() != 32) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 16; ++i) {
        const int hi = nibble(hex[static_cast<std::size_t>(2 * i)]);
        const int lo = nibble(hex[static_cast<std::size_t>(2 * i + 1)]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<char8>((hi << 4) | lo);
    }
    return true;
}

template <typename T>
void release(T*& p) noexcept {
    if (p != nullptr) {
        p->release();
        p = nullptr;
    }
}

// ---- module ---------------------------------------------------------------------

// A loaded plug-in binary. Shared by every instance created from it and
// unloaded (after the factory is released) when the last instance goes away.
class Module {
public:
    static Status load(const std::string& path, std::shared_ptr<Module>& out) {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<Module>> cache;
        std::lock_guard lock(mutex);
        if (auto existing = cache[path].lock()) {
            out = existing;
            return Status::ok();
        }
        auto m = std::shared_ptr<Module>(new Module());
        if (Status st = m->open(path); !st.is_ok()) return st;
        cache[path] = m;
        out = m;
        return Status::ok();
    }
    ~Module() {
        release(factory_);
#if defined(_WIN32)
        if (handle_ != nullptr) {
            using ExitFn = bool (*)();
            if (auto exit_dll = reinterpret_cast<ExitFn>(reinterpret_cast<void*>(::GetProcAddress(handle_, "ExitDll")))) exit_dll();
            ::FreeLibrary(handle_);
        }
#else
        if (handle_ != nullptr) {
            using ExitFn = bool (*)();
            if (auto exit_module = reinterpret_cast<ExitFn>(::dlsym(handle_, "ModuleExit"))) exit_module();
            ::dlclose(handle_);
        }
#endif
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    [[nodiscard]] IPluginFactory* factory() const noexcept { return factory_; }

private:
    Module() = default;

    static fs::path binary_of(const fs::path& bundle) {
        std::error_code ec;
        if (!fs::is_directory(bundle, ec)) return bundle;  // single-file .vst3 (Windows legacy)
#if defined(_WIN32)
        return bundle / "Contents" / "x86_64-win" / (bundle.stem().wstring() + L".vst3");
#else
        return bundle / "Contents" / "x86_64-linux" / (bundle.stem().string() + ".so");
#endif
    }

    Status open(const std::string& path) {
        const fs::path bin = binary_of(fs::path(path));
        std::error_code ec;
        if (!fs::exists(bin, ec)) return {MIXIT_E_NOT_FOUND, "plug-in binary not found: " + bin.string()};
        using FactoryFn = IPluginFactory* (*)();
#if defined(_WIN32)
        handle_ = ::LoadLibraryExW(bin.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (handle_ == nullptr) return {MIXIT_E_IO, "cannot load " + bin.string() + " (error " + std::to_string(::GetLastError()) + ")"};
        using InitFn = bool (*)();
        if (auto init = reinterpret_cast<InitFn>(reinterpret_cast<void*>(::GetProcAddress(handle_, "InitDll")))) {
            if (!init()) return {MIXIT_E_INTERNAL, "InitDll failed"};
        }
        auto get = reinterpret_cast<FactoryFn>(reinterpret_cast<void*>(::GetProcAddress(handle_, "GetPluginFactory")));
#else
        handle_ = ::dlopen(bin.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle_ == nullptr) {
            const char* err = ::dlerror();
            return {MIXIT_E_IO, std::string("cannot load plug-in: ") + (err != nullptr ? err : "unknown error")};
        }
        using EntryFn = bool (*)(void*);
        if (auto entry = reinterpret_cast<EntryFn>(::dlsym(handle_, "ModuleEntry"))) {
            if (!entry(handle_)) return {MIXIT_E_INTERNAL, "ModuleEntry failed"};
        }
        auto get = reinterpret_cast<FactoryFn>(::dlsym(handle_, "GetPluginFactory"));
#endif
        if (get == nullptr) return {MIXIT_E_NOT_SUPPORTED, "not a VST3 plug-in (no GetPluginFactory)"};
        factory_ = get();
        if (factory_ == nullptr) return {MIXIT_E_INTERNAL, "GetPluginFactory returned NULL"};
        return Status::ok();
    }

#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    IPluginFactory* factory_ = nullptr;
};

// ---- host-side interface implementations (owned by the effect; not refcounted) ----

#define MIXIT_VST3_NO_REFCOUNT                                    \
    uint32 PLUGIN_API addRef() override { return 1; }            \
    uint32 PLUGIN_API release() override { return 1; }

class HostApplication final : public IHostApplication {
public:
    tresult PLUGIN_API getName(String128 name) override {
        static constexpr char kName[] = "MIX IT";
        for (std::size_t i = 0; i < sizeof(kName); ++i) name[i] = static_cast<char16>(kName[i]);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;  // host messages / attribute lists: not provided yet
    }
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, IHostApplication::iid) || FUnknownPrivate::iidEqual(wanted, FUnknown::iid)) {
            *obj = static_cast<IHostApplication*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MIXIT_VST3_NO_REFCOUNT
};

class MemoryStream final : public IBStream {
public:
    std::vector<std::uint8_t> data;
    std::size_t pos = 0;

    tresult PLUGIN_API read(void* buffer, int32 bytes, int32* done) override {
        const std::size_t n = bytes <= 0 ? 0 : std::min<std::size_t>(static_cast<std::size_t>(bytes), data.size() - pos);
        if (n > 0) std::memcpy(buffer, data.data() + pos, n);
        pos += n;
        if (done != nullptr) *done = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 bytes, int32* done) override {
        if (bytes < 0) return kInvalidArgument;
        try {
            if (pos + static_cast<std::size_t>(bytes) > data.size()) data.resize(pos + static_cast<std::size_t>(bytes));
        } catch (...) {
            return kOutOfMemory;
        }
        if (bytes > 0) std::memcpy(data.data() + pos, buffer, static_cast<std::size_t>(bytes));
        pos += static_cast<std::size_t>(bytes);
        if (done != nullptr) *done = bytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 mode, int64* result) override {
        int64 base = 0;
        if (mode == kIBSeekCur) base = static_cast<int64>(pos);
        if (mode == kIBSeekEnd) base = static_cast<int64>(data.size());
        const int64 target = base + offset;
        if (target < 0) return kInvalidArgument;
        pos = static_cast<std::size_t>(target);
        if (pos > data.size()) data.resize(pos);
        if (result != nullptr) *result = target;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* result) override {
        if (result != nullptr) *result = static_cast<int64>(pos);
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, IBStream::iid) || FUnknownPrivate::iidEqual(wanted, FUnknown::iid)) {
            *obj = static_cast<IBStream*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MIXIT_VST3_NO_REFCOUNT
};

// Parameter changes for one block: fixed storage, filled on the audio thread.
class ParamQueue final : public IParamValueQueue {
public:
    static constexpr int kMaxPoints = 8;
    ParamID id = 0;
    int32 count = 0;
    std::array<std::pair<int32, ParamValue>, kMaxPoints> points{};

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override {
        if (index < 0 || index >= count) return kInvalidArgument;
        offset = points[static_cast<std::size_t>(index)].first;
        value = points[static_cast<std::size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override {
        if (count >= kMaxPoints) return kResultFalse;
        points[static_cast<std::size_t>(count)] = {offset, value};
        index = count++;
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, IParamValueQueue::iid) || FUnknownPrivate::iidEqual(wanted, FUnknown::iid)) {
            *obj = static_cast<IParamValueQueue*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MIXIT_VST3_NO_REFCOUNT
};

class ParamChanges final : public IParameterChanges {
public:
    explicit ParamChanges(std::size_t capacity) : queues_(capacity) {}
    void clear() noexcept { used_ = 0; }
    int32 PLUGIN_API getParameterCount() override { return used_; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < used_ ? &queues_[static_cast<std::size_t>(index)] : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        for (int32 i = 0; i < used_; ++i) {
            if (queues_[static_cast<std::size_t>(i)].id == id) {
                index = i;
                return &queues_[static_cast<std::size_t>(i)];
            }
        }
        if (static_cast<std::size_t>(used_) >= queues_.size()) return nullptr;
        ParamQueue& q = queues_[static_cast<std::size_t>(used_)];
        q.id = id;
        q.count = 0;
        index = used_++;
        return &q;
    }
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, IParameterChanges::iid) || FUnknownPrivate::iidEqual(wanted, FUnknown::iid)) {
            *obj = static_cast<IParameterChanges*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MIXIT_VST3_NO_REFCOUNT

private:
    std::vector<ParamQueue> queues_;
    int32 used_ = 0;
};

// Latest-value table of parameter changes from the control thread (and
// plug-in editors) to the audio thread: one slot per parameter, so it can
// never overflow, and only the newest value of a parameter is sent per block.
class ParamFeed {
public:
    void init(const std::vector<Vst3ParameterInfo>& params) {
        ids_.clear();
        for (const Vst3ParameterInfo& p : params) ids_.push_back(p.id);
        values_ = std::make_unique<std::atomic<double>[]>(ids_.size());
        dirty_ = std::make_unique<std::atomic<bool>[]>(ids_.size());
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            values_[i].store(0.0, std::memory_order_relaxed);
            dirty_[i].store(false, std::memory_order_relaxed);
        }
    }
    // Any thread. Unknown ids are ignored (returns false).
    bool set(ParamID id, ParamValue value) noexcept {
        const auto it = std::find(ids_.begin(), ids_.end(), id);
        if (it == ids_.end()) return false;
        const auto i = static_cast<std::size_t>(it - ids_.begin());
        values_[i].store(value, std::memory_order_relaxed);
        dirty_[i].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
        return true;
    }
    // Audio thread: moves pending values into `changes`.
    template <typename Changes>
    void drain(Changes& changes) noexcept {
        if (!any_.exchange(false, std::memory_order_acq_rel)) return;
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (!dirty_[i].exchange(false, std::memory_order_acq_rel)) continue;
            int32 index = 0;
            if (IParamValueQueue* q = changes.addParameterData(ids_[i], index)) {
                int32 point = 0;
                q->addPoint(0, values_[i].load(std::memory_order_relaxed), point);
            }
        }
    }

private:
    std::vector<ParamID> ids_;
    std::unique_ptr<std::atomic<double>[]> values_;
    std::unique_ptr<std::atomic<bool>[]> dirty_;
    std::atomic<bool> any_{false};
};

class ComponentHandler final : public IComponentHandler {
public:
    explicit ComponentHandler(ParamFeed& feed) : feed_(feed) {}
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        return feed_.set(id, value) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kNotImplemented; }
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override {
        if (FUnknownPrivate::iidEqual(wanted, IComponentHandler::iid) || FUnknownPrivate::iidEqual(wanted, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    MIXIT_VST3_NO_REFCOUNT

private:
    ParamFeed& feed_;
};

}  // namespace

// ---- effect ---------------------------------------------------------------------

struct Vst3Effect::Impl {
    std::shared_ptr<Module> module;
    HostApplication host;
    ParamFeed feed;
    ComponentHandler handler{feed};
    IComponent* component = nullptr;
    IAudioProcessor* processor = nullptr;
    IEditController* controller = nullptr;
    bool separate_controller = false;
    IConnectionPoint* component_cp = nullptr;
    IConnectionPoint* controller_cp = nullptr;
    bool active = false;
    bool processing = false;
    Vst3PluginInfo info;
    std::vector<Vst3ParameterInfo> parameters;
    std::uint32_t channels = 2;
    std::uint32_t max_block = 480;
    int32 input_buses = 0;
    // Audio thread storage.
    std::vector<float> in_storage, out_storage;
    std::vector<float*> in_ptrs, out_ptrs;
    std::unique_ptr<ParamChanges> changes;

    ~Impl() {
        try {
            if (processing && processor != nullptr) processor->setProcessing(false);
            if (active && component != nullptr) component->setActive(false);
            if (component_cp != nullptr && controller_cp != nullptr) {
                component_cp->disconnect(controller_cp);
                controller_cp->disconnect(component_cp);
            }
            release(component_cp);
            release(controller_cp);
            if (controller != nullptr) {
                controller->setComponentHandler(nullptr);
                if (separate_controller) controller->terminate();
            }
            release(controller);
            release(processor);
            if (component != nullptr) component->terminate();
            release(component);
        } catch (...) {
            // A plug-in must not throw across its interface; nothing more to do here.
        }
    }
};

Vst3Effect::Vst3Effect() : impl_(std::make_unique<Impl>()) {}
Vst3Effect::~Vst3Effect() = default;

const Vst3PluginInfo& Vst3Effect::info() const noexcept { return impl_->info; }
const std::vector<Vst3ParameterInfo>& Vst3Effect::parameters() const noexcept { return impl_->parameters; }

std::uint32_t Vst3Effect::latency_samples() const noexcept {
    try {
        return impl_->processor != nullptr ? impl_->processor->getLatencySamples() : 0;
    } catch (...) {
        return 0;
    }
}

Status Vst3Effect::create(const std::string& path, const std::string& class_id, double sample_rate,
                          std::uint32_t channels, std::uint32_t max_block, std::unique_ptr<Vst3Effect>& out) {
    if (channels < 1 || channels > 2) return {MIXIT_E_NOT_SUPPORTED, "VST3 inserts support mono and stereo sessions"};
    TUID cid;
    if (!hex_to_cid(class_id, cid)) return {MIXIT_E_INVALID_ARGUMENT, "class id must be 32 hex digits"};
    std::unique_ptr<Vst3Effect> fx(new Vst3Effect());
    Impl& d = *fx->impl_;
    d.channels = channels;
    d.max_block = max_block;
    if (Status st = Module::load(path, d.module); !st.is_ok()) return st;
    IPluginFactory* factory = d.module->factory();
    try {
        // Class info (for the operator) and instance.
        bool found = false;
        PFactoryInfo fi{};
        if (factory->getFactoryInfo(&fi) == kResultOk) d.info.vendor = bounded(fi.vendor, PFactoryInfo::kNameSize);
        for (int32 i = 0; i < factory->countClasses(); ++i) {
            PClassInfo ci{};
            if (factory->getClassInfo(i, &ci) != kResultOk || std::memcmp(ci.cid, cid, sizeof(TUID)) != 0) continue;
            found = true;
            d.info.name = bounded(ci.name, PClassInfo::kNameSize);
        }
        if (!found) return {MIXIT_E_NOT_FOUND, "the plug-in has no class " + class_id};
        d.info.path = path;
        d.info.class_id = class_id;
        if (factory->createInstance(cid, IComponent::iid, reinterpret_cast<void**>(&d.component)) != kResultOk ||
            d.component == nullptr) {
            return {MIXIT_E_INTERNAL, "the plug-in did not create its component"};
        }
        if (d.component->initialize(&d.host) != kResultOk) return {MIXIT_E_INTERNAL, "component initialize failed"};

        // Edit controller: a separate class, or the component itself.
        TUID controller_cid;
        if (d.component->getControllerClassId(controller_cid) == kResultTrue &&
            factory->createInstance(controller_cid, IEditController::iid, reinterpret_cast<void**>(&d.controller)) == kResultOk &&
            d.controller != nullptr) {
            d.separate_controller = true;
            if (d.controller->initialize(&d.host) != kResultOk) return {MIXIT_E_INTERNAL, "controller initialize failed"};
            if (d.component->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&d.component_cp)) == kResultOk &&
                d.controller->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&d.controller_cp)) == kResultOk) {
                d.component_cp->connect(d.controller_cp);
                d.controller_cp->connect(d.component_cp);
            }
        } else {
            d.controller = nullptr;
            (void)d.component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&d.controller));
        }
        if (d.controller != nullptr) {
            MemoryStream state;
            if (d.component->getState(&state) == kResultOk) {
                state.pos = 0;
                d.controller->setComponentState(&state);
            }
            d.controller->setComponentHandler(&d.handler);
            const int32 n = d.controller->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                ParameterInfo pi{};
                if (d.controller->getParameterInfo(i, pi) != kResultOk) continue;
                Vst3ParameterInfo p;
                p.id = pi.id;
                p.title = to_utf8(pi.title, 128);
                p.units = to_utf8(pi.units, 128);
                p.step_count = pi.stepCount;
                p.default_normalized = pi.defaultNormalizedValue;
                p.read_only = (pi.flags & ParameterInfo::kIsReadOnly) != 0;
                p.is_bypass = (pi.flags & ParameterInfo::kIsBypass) != 0;
                d.parameters.push_back(std::move(p));
            }
        }

        // Audio processing setup.
        if (d.component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&d.processor)) != kResultOk ||
            d.processor == nullptr) {
            return {MIXIT_E_NOT_SUPPORTED, "the plug-in is not an audio processor"};
        }
        if (d.processor->canProcessSampleSize(kSample32) != kResultTrue) {
            return {MIXIT_E_NOT_SUPPORTED, "the plug-in cannot process 32-bit float"};
        }
        d.input_buses = d.component->getBusCount(kAudio, kInput);
        const int32 outputs = d.component->getBusCount(kAudio, kOutput);
        if (d.input_buses < 1 || outputs < 1) return {MIXIT_E_NOT_SUPPORTED, "the plug-in has no audio input/output bus"};
        const SpeakerArrangement arr = channels == 2 ? SpeakerArr::kStereo : SpeakerArr::kMono;
        std::vector<SpeakerArrangement> ins(static_cast<std::size_t>(d.input_buses), arr);
        std::vector<SpeakerArrangement> outs(static_cast<std::size_t>(outputs), arr);
        if (d.processor->setBusArrangements(ins.data(), d.input_buses, outs.data(), outputs) != kResultTrue) {
            SpeakerArrangement in0 = 0, out0 = 0;
            d.processor->getBusArrangement(kInput, 0, in0);
            d.processor->getBusArrangement(kOutput, 0, out0);
            if (in0 != arr || out0 != arr) {
                return {MIXIT_E_NOT_SUPPORTED, channels == 2 ? "the plug-in does not accept stereo in/out"
                                                             : "the plug-in does not accept mono in/out"};
            }
        }
        d.component->activateBus(kAudio, kInput, 0, true);
        d.component->activateBus(kAudio, kOutput, 0, true);
        ProcessSetup setup{kRealtime, kSample32, static_cast<int32>(max_block), sample_rate};
        if (d.processor->setupProcessing(setup) != kResultOk) return {MIXIT_E_NOT_SUPPORTED, "setupProcessing refused"};
        if (d.component->setActive(true) != kResultOk) return {MIXIT_E_INTERNAL, "setActive failed"};
        d.active = true;
        const tresult pr = d.processor->setProcessing(true);
        d.processing = pr == kResultOk || pr == kResultTrue;
    } catch (...) {
        return {MIXIT_E_INTERNAL, "the plug-in threw an exception during set-up"};
    }

    const std::size_t floats = static_cast<std::size_t>(channels) * max_block;
    d.in_storage.assign(floats, 0.0f);
    d.out_storage.assign(floats, 0.0f);
    for (std::uint32_t c = 0; c < channels; ++c) {
        d.in_ptrs.push_back(d.in_storage.data() + static_cast<std::size_t>(c) * max_block);
        d.out_ptrs.push_back(d.out_storage.data() + static_cast<std::size_t>(c) * max_block);
    }
    d.feed.init(d.parameters);
    d.changes = std::make_unique<ParamChanges>(std::max<std::size_t>(d.parameters.size(), 1));
    out = std::move(fx);
    return Status::ok();
}

void Vst3Effect::process(float* io, std::uint32_t frames) noexcept {
    Impl& d = *impl_;
    // Parameter changes are delivered even while bypassed so the plug-in
    // resumes with the operator's latest settings.
    d.changes->clear();
    d.feed.drain(*d.changes);
    if (frames == 0 || bypass_.load(std::memory_order_relaxed) || frames > d.max_block) {
        if (d.changes->getParameterCount() > 0 && frames <= d.max_block) {
            // Flush the changes with a zero-length block (allowed by VST3).
            ProcessData flush;
            flush.processMode = kRealtime;
            flush.symbolicSampleSize = kSample32;
            flush.numSamples = 0;
            flush.inputParameterChanges = d.changes.get();
            try {
                (void)d.processor->process(flush);
            } catch (...) {
                process_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    const std::uint32_t ch = d.channels;
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t k = 0; k < ch; ++k) d.in_ptrs[k][i] = io[static_cast<std::size_t>(i) * ch + k];
    }
    AudioBusBuffers in{};
    in.numChannels = static_cast<int32>(ch);
    in.channelBuffers32 = d.in_ptrs.data();
    AudioBusBuffers out{};
    out.numChannels = static_cast<int32>(ch);
    out.channelBuffers32 = d.out_ptrs.data();
    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = static_cast<int32>(frames);
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &in;
    data.outputs = &out;
    data.inputParameterChanges = d.changes.get();
    tresult r = kResultFalse;
    try {
        r = d.processor->process(data);
    } catch (...) {
        r = kInternalError;
    }
    if (r != kResultOk) {
        process_errors_.fetch_add(1, std::memory_order_relaxed);
        return;  // pass the input through unchanged
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t k = 0; k < ch; ++k) io[static_cast<std::size_t>(i) * ch + k] = d.out_ptrs[k][i];
    }
}

Status Vst3Effect::set_parameter(std::uint32_t id, double normalized) {
    if (!(normalized >= 0.0 && normalized <= 1.0)) return {MIXIT_E_INVALID_ARGUMENT, "normalized value must be 0..1"};
    const auto it = std::find_if(impl_->parameters.begin(), impl_->parameters.end(),
                                 [id](const Vst3ParameterInfo& p) { return p.id == id; });
    if (it == impl_->parameters.end()) return {MIXIT_E_NOT_FOUND, "no parameter " + std::to_string(id)};
    if (it->read_only) return {MIXIT_E_INVALID_ARGUMENT, "parameter is read-only"};
    try {
        if (impl_->controller != nullptr) impl_->controller->setParamNormalized(id, normalized);
    } catch (...) {
        return {MIXIT_E_INTERNAL, "the plug-in threw in setParamNormalized"};
    }
    impl_->feed.set(id, normalized);
    return Status::ok();
}

Status Vst3Effect::get_parameter(std::uint32_t id, double& normalized) const {
    if (impl_->controller == nullptr) return {MIXIT_E_NOT_SUPPORTED, "the plug-in has no edit controller"};
    try {
        normalized = impl_->controller->getParamNormalized(id);
    } catch (...) {
        return {MIXIT_E_INTERNAL, "the plug-in threw in getParamNormalized"};
    }
    return Status::ok();
}

std::string Vst3Effect::parameter_text(std::uint32_t id, double normalized) const {
    if (impl_->controller == nullptr) return {};
    String128 text{};
    try {
        if (impl_->controller->getParamStringByValue(id, normalized, text) != kResultOk) return {};
    } catch (...) {
        return {};
    }
    return to_utf8(text, 128);
}

Status Vst3Effect::get_state(std::vector<std::uint8_t>& out) const {
    MemoryStream comp, ctrl;
    try {
        if (impl_->component->getState(&comp) != kResultOk) return {MIXIT_E_INTERNAL, "component getState failed"};
        if (impl_->controller != nullptr) (void)impl_->controller->getState(&ctrl);
    } catch (...) {
        return {MIXIT_E_INTERNAL, "the plug-in threw in getState"};
    }
    out.clear();
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    out.insert(out.end(), {'M', 'X', 'V', '3'});
    put32(static_cast<std::uint32_t>(comp.data.size()));
    out.insert(out.end(), comp.data.begin(), comp.data.end());
    put32(static_cast<std::uint32_t>(ctrl.data.size()));
    out.insert(out.end(), ctrl.data.begin(), ctrl.data.end());
    return Status::ok();
}

Status Vst3Effect::set_state(const std::uint8_t* data, std::size_t size) {
    auto get32 = [&](std::size_t at, std::uint32_t& v) {
        if (at + 4 > size) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        return true;
    };
    std::uint32_t comp_size = 0, ctrl_size = 0;
    if (size < 12 || std::memcmp(data, "MXV3", 4) != 0 || !get32(4, comp_size) || 8 + std::size_t{comp_size} + 4 > size ||
        !get32(8 + comp_size, ctrl_size) || 12 + std::size_t{comp_size} + ctrl_size != size) {
        return {MIXIT_E_INVALID_ARGUMENT, "not a MIX IT VST3 state blob"};
    }
    MemoryStream comp, ctrl;
    comp.data.assign(data + 8, data + 8 + comp_size);
    ctrl.data.assign(data + 12 + comp_size, data + size);
    try {
        if (impl_->component->setState(&comp) != kResultOk) return {MIXIT_E_INTERNAL, "component setState failed"};
        if (impl_->controller != nullptr) {
            comp.pos = 0;
            impl_->controller->setComponentState(&comp);
            if (!ctrl.data.empty()) impl_->controller->setState(&ctrl);
        }
    } catch (...) {
        return {MIXIT_E_INTERNAL, "the plug-in threw in setState"};
    }
    return Status::ok();
}

// ---- scanning -------------------------------------------------------------------

namespace {

void scan_bundle(const fs::path& bundle, std::vector<Vst3PluginInfo>& out, std::vector<std::string>& errors) {
    std::shared_ptr<Module> module;
    if (Status st = Module::load(bundle.string(), module); !st.is_ok()) {
        errors.push_back(bundle.string() + ": " + st.message());
        return;
    }
    IPluginFactory* f = module->factory();
    try {
        PFactoryInfo fi{};
        std::string vendor;
        if (f->getFactoryInfo(&fi) == kResultOk) vendor = bounded(fi.vendor, PFactoryInfo::kNameSize);
        IPluginFactory2* f2 = nullptr;
        (void)f->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&f2));
        for (int32 i = 0; i < f->countClasses(); ++i) {
            PClassInfo ci{};
            if (f->getClassInfo(i, &ci) != kResultOk) continue;
            if (std::strncmp(ci.category, kVstAudioEffectClass, PClassInfo::kCategorySize) != 0) continue;
            Vst3PluginInfo p;
            p.path = bundle.string();
            p.class_id = cid_to_hex(ci.cid);
            p.name = bounded(ci.name, PClassInfo::kNameSize);
            p.vendor = vendor;
            PClassInfo2 c2{};
            if (f2 != nullptr && f2->getClassInfo2(i, &c2) == kResultOk) {
                p.subcategories = bounded(c2.subCategories, PClassInfo2::kSubCategoriesSize);
                p.version = bounded(c2.version, PClassInfo2::kVersionSize);
                const std::string v = bounded(c2.vendor, PClassInfo2::kVendorSize);
                if (!v.empty()) p.vendor = v;
            }
            out.push_back(std::move(p));
        }
        release(f2);
    } catch (...) {
        errors.push_back(bundle.string() + ": the plug-in threw an exception while being scanned");
    }
}

void scan_folder(const fs::path& dir, int depth, std::vector<Vst3PluginInfo>& out, std::vector<std::string>& errors) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (p.extension() == ".vst3") {
            scan_bundle(p, out, errors);
        } else if (depth < 3 && it->is_directory(ec)) {
            scan_folder(p, depth + 1, out, errors);
        }
    }
}

}  // namespace

Status vst3_scan(const std::string& path, std::vector<Vst3PluginInfo>& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    const fs::path p(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return {MIXIT_E_NOT_FOUND, "no such file or folder: " + path};
    try {
        if (p.extension() == ".vst3") {
            scan_bundle(p, out, errors);
        } else if (fs::is_directory(p, ec)) {
            scan_folder(p, 0, out, errors);
        } else {
            return {MIXIT_E_INVALID_ARGUMENT, "expected a .vst3 bundle or a folder"};
        }
    } catch (const std::exception& e) {
        return {MIXIT_E_IO, std::string("scan failed: ") + e.what()};
    }
    return Status::ok();
}

std::vector<std::string> vst3_default_folders() {
    std::vector<std::string> folders;
    std::error_code ec;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetEnvironmentVariableW(L"CommonProgramFiles", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const fs::path common = fs::path(buf) / "VST3";
        if (fs::is_directory(common, ec)) folders.push_back(common.string());
    }
#else
    if (const char* home = std::getenv("HOME")) {
        const fs::path user = fs::path(home) / ".vst3";
        if (fs::is_directory(user, ec)) folders.push_back(user.string());
    }
    for (const char* sys : {"/usr/lib/vst3", "/usr/local/lib/vst3"}) {
        if (fs::is_directory(sys, ec)) folders.emplace_back(sys);
    }
#endif
    return folders;
}

}  // namespace mixit::audio

#else  // VST3 support not built

namespace mixit::audio {

Status vst3_scan(const std::string&, std::vector<Vst3PluginInfo>& out, std::vector<std::string>& errors) {
    out.clear();
    errors.clear();
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without VST3 support"};
}
std::vector<std::string> vst3_default_folders() { return {}; }

struct Vst3Effect::Impl {};
Vst3Effect::Vst3Effect() = default;
Vst3Effect::~Vst3Effect() = default;
Status Vst3Effect::create(const std::string&, const std::string&, double, std::uint32_t, std::uint32_t,
                          std::unique_ptr<Vst3Effect>&) {
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without VST3 support"};
}
void Vst3Effect::process(float*, std::uint32_t) noexcept {}
const Vst3PluginInfo& Vst3Effect::info() const noexcept {
    static const Vst3PluginInfo empty;
    return empty;
}
const std::vector<Vst3ParameterInfo>& Vst3Effect::parameters() const noexcept {
    static const std::vector<Vst3ParameterInfo> empty;
    return empty;
}
Status Vst3Effect::set_parameter(std::uint32_t, double) { return {MIXIT_E_NOT_SUPPORTED, "no VST3 support"}; }
Status Vst3Effect::get_parameter(std::uint32_t, double&) const { return {MIXIT_E_NOT_SUPPORTED, "no VST3 support"}; }
std::string Vst3Effect::parameter_text(std::uint32_t, double) const { return {}; }
std::uint32_t Vst3Effect::latency_samples() const noexcept { return 0; }
Status Vst3Effect::get_state(std::vector<std::uint8_t>&) const { return {MIXIT_E_NOT_SUPPORTED, "no VST3 support"}; }
Status Vst3Effect::set_state(const std::uint8_t*, std::size_t) { return {MIXIT_E_NOT_SUPPORTED, "no VST3 support"}; }

}  // namespace mixit::audio

#endif
