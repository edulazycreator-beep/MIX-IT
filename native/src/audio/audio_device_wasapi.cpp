// WASAPI (Windows Core Audio) backend, shared mode. Public, documented APIs
// only: IMMDeviceEnumerator, IAudioClient, IAudioRenderClient,
// IAudioCaptureClient, and AvSetMmThreadCharacteristicsW (avrt.dll, loaded at
// run time) for the "Pro Audio" scheduling class.
//
// Streams are float32 at the session's rate and channel count; Windows
// converts to the device's mix format (AUTOCONVERTPCM). The device clock is
// bridged to the master clock by the DriftResampler on our side.
//
// EXPERIMENTAL: compiled and exercised on Windows CI without audio endpoints;
// not yet verified with real devices.
#include "audio/audio_device.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <thread>

#include "audio/capture_pacer.hpp"
#include "logging/logger.hpp"
#include "platform/com_ref.hpp"
#include "platform/os.hpp"
#include "platform/win_strings.hpp"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace mixit::audio {
namespace {

// PKEY_Device_FriendlyName and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here
// so no GUID library is needed (values from the Windows SDK headers).
constexpr PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr GUID kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string hr_text(HRESULT hr) { return std::format("0x{:08X}", static_cast<unsigned>(hr)); }

class ComScope {
public:
    ComScope() : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// Callers may be STA UI threads: COM work runs on a short-lived MTA thread.
Status on_mta_thread(const std::function<Status()>& fn) {
    Status result{MIXIT_E_INTERNAL, "audio helper thread did not run"};
    try {
        std::thread t([&] {
            ComScope com;
            result = com.ok() ? fn() : Status{MIXIT_E_PLATFORM, "COM initialisation failed"};
        });
        t.join();
    } catch (const std::system_error& e) {
        return {MIXIT_E_PLATFORM, std::string("cannot start audio helper thread: ") + e.what()};
    }
    return result;
}

Status enumerator(ComRef<IMMDeviceEnumerator>& out) {
    const HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(out.put()));
    if (FAILED(hr)) return {MIXIT_E_NOT_SUPPORTED, "Windows audio is not available (" + hr_text(hr) + ")"};
    return Status::ok();
}

std::string device_id(IMMDevice* d) {
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id)) || id == nullptr) return {};
    std::string s = os::wide_to_utf8(id);
    ::CoTaskMemFree(id);
    return s;
}

std::string friendly_name(IMMDevice* d) {
    ComRef<IPropertyStore> props;
    if (FAILED(d->OpenPropertyStore(STGM_READ, props.put()))) return {};
    PROPVARIANT v;
    ::PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal != nullptr) {
        name = os::wide_to_utf8(v.pwszVal);
    }
    ::PropVariantClear(&v);
    return name;
}

Status find_device(IMMDeviceEnumerator* e, DeviceDirection dir, const std::string& id, ComRef<IMMDevice>& out) {
    HRESULT hr;
    if (id.empty()) {
        hr = e->GetDefaultAudioEndpoint(dir == DeviceDirection::Render ? eRender : eCapture, eConsole, out.put());
        if (FAILED(hr)) return {MIXIT_E_NOT_FOUND, "no default audio device (" + hr_text(hr) + ")"};
    } else {
        const std::wstring wide = os::utf8_to_wide(id);
        hr = e->GetDevice(wide.c_str(), out.put());
        if (FAILED(hr)) return {MIXIT_E_NOT_FOUND, "audio device not found (" + hr_text(hr) + ")"};
    }
    DWORD state = 0;
    if (FAILED(out->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
        return {MIXIT_E_NOT_FOUND, "audio device is not active (disabled or unplugged)"};
    }
    return Status::ok();
}

WAVEFORMATEXTENSIBLE float_format(std::uint32_t sample_rate, std::uint32_t channels) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = sample_rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    f.Format.nAvgBytesPerSec = sample_rate * channels * 4;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    f.SubFormat = kSubtypeFloat;
    return f;
}

// "Pro Audio" MMCSS class for the device thread (best effort).
class MmcssScope {
public:
    MmcssScope() {
        HMODULE avrt = ::LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (avrt == nullptr) return;
        using SetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
        using RevertFn = BOOL(WINAPI*)(HANDLE);
        const auto set = reinterpret_cast<SetFn>(reinterpret_cast<void*>(::GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
        revert_ = reinterpret_cast<RevertFn>(reinterpret_cast<void*>(::GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")));
        DWORD index = 0;
        if (set != nullptr) handle_ = set(L"Pro Audio", &index);
    }
    ~MmcssScope() {
        if (handle_ != nullptr && revert_ != nullptr) revert_(handle_);
    }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
    BOOL(WINAPI* revert_)(HANDLE) = nullptr;
};

// ---- render --------------------------------------------------------------------

class WasapiOutput final : public AudioOutput {
public:
    WasapiOutput(const AudioOutputConfig& cfg, SampleRing& ring, Logger& logger)
        : cfg_(cfg), ring_(ring), logger_(logger),
          resampler_(cfg.channels, std::max<std::uint32_t>(1, cfg.sample_rate * cfg.latency_ms / 1000)) {
        event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = std::thread([this] { loop(); });
    }
    ~WasapiOutput() override {
        stop_.store(true);
        if (event_ != nullptr) ::SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        if (event_ != nullptr) ::CloseHandle(event_);
    }

    [[nodiscard]] AudioOutputStats stats() const override {
        AudioOutputStats s;
        s.state = static_cast<OutputState>(state_.load());
        s.underruns = underruns_.load();
        s.device_errors = errors_.load();
        s.ratio = ratio_.load();
        s.buffered_frames = ring_.available();
        std::lock_guard lock(name_mutex_);
        s.device_name = name_;
        return s;
    }

private:
    void loop() noexcept {
        os::set_current_thread_name("mixit-aout");
        ComScope com;
        MmcssScope mmcss;
        bool reported = false;
        while (!stop_.load()) {
            const Status st = com.ok() ? play() : Status{MIXIT_E_PLATFORM, "COM initialisation failed"};
            if (stop_.load()) break;
            errors_.fetch_add(1);
            state_.store(static_cast<int>(OutputState::Retrying));
            if (!reported) {
                logger_.logf(MIXIT_LOG_ERROR, "audio", "monitor output: {} — retrying every second", st.message());
                reported = true;
            }
            ring_.clear();
            for (int i = 0; i < 10 && !stop_.load(); ++i) ::Sleep(100);
        }
        state_.store(static_cast<int>(OutputState::Off));
    }

    // Opens the device and plays until stop or a device error.
    Status play() noexcept {
        ComRef<IMMDeviceEnumerator> e;
        if (Status st = enumerator(e); !st.is_ok()) return st;
        ComRef<IMMDevice> device;
        if (Status st = find_device(e.get(), DeviceDirection::Render, cfg_.device_id, device); !st.is_ok()) return st;
        {
            std::lock_guard lock(name_mutex_);
            name_ = friendly_name(device.get());
        }
        ComRef<IAudioClient> client;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
        if (FAILED(hr)) return {MIXIT_E_PLATFORM, "IAudioClient activation failed (" + hr_text(hr) + ")"};
        WAVEFORMATEXTENSIBLE fmt = float_format(cfg_.sample_rate, cfg_.channels);
        const REFERENCE_TIME buffer = 200'000;  // 20 ms in 100 ns units
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                buffer, 0, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) return {MIXIT_E_NOT_SUPPORTED, "the device refused the stream format (" + hr_text(hr) + ")"};
        if (FAILED(hr = client->SetEventHandle(event_))) return {MIXIT_E_PLATFORM, "SetEventHandle " + hr_text(hr)};
        UINT32 frames = 0;
        if (FAILED(hr = client->GetBufferSize(&frames))) return {MIXIT_E_PLATFORM, "GetBufferSize " + hr_text(hr)};
        ComRef<IAudioRenderClient> render;
        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.put()));
        if (FAILED(hr)) return {MIXIT_E_PLATFORM, "IAudioRenderClient " + hr_text(hr)};
        resampler_.reset();
        if (FAILED(hr = client->Start())) return {MIXIT_E_PLATFORM, "IAudioClient::Start " + hr_text(hr)};
        state_.store(static_cast<int>(OutputState::Running));
        logger_.logf(MIXIT_LOG_INFO, "audio", "monitor output on '{}' ({} frames device buffer)", name_, frames);

        Status result = Status::ok();
        while (!stop_.load()) {
            ::WaitForSingleObject(event_, 200);
            UINT32 padding = 0;
            if (FAILED(hr = client->GetCurrentPadding(&padding))) {
                result = {MIXIT_E_IO, "device lost (" + hr_text(hr) + ")"};
                break;
            }
            const UINT32 avail = frames - std::min(padding, frames);
            if (avail == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(hr = render->GetBuffer(avail, &data))) {
                result = {MIXIT_E_IO, "device lost (" + hr_text(hr) + ")"};
                break;
            }
            resampler_.pull(ring_, reinterpret_cast<float*>(data), avail);
            render->ReleaseBuffer(avail, 0);
            underruns_.store(resampler_.stats().underruns);
            ratio_.store(resampler_.stats().ratio);
        }
        client->Stop();
        return result;
    }

    AudioOutputConfig cfg_;
    SampleRing& ring_;
    Logger& logger_;
    DriftResampler resampler_;
    HANDLE event_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<int> state_{static_cast<int>(OutputState::Off)};
    std::atomic<std::uint64_t> underruns_{0}, errors_{0};
    std::atomic<double> ratio_{1.0};
    mutable std::mutex name_mutex_;
    std::string name_;
    std::thread thread_;
};

// ---- capture -------------------------------------------------------------------

class WasapiCapture final : public CapturePacer {
public:
    WasapiCapture(std::string id, std::string name, Logger& logger) : id_(std::move(id)), logger_(logger) {
        format_.has_audio = true;
        format_.has_video = false;
        format_.description = "audio capture '" + name + "' (EXPERIMENTAL)";
    }
    [[nodiscard]] const media::SourceFormat& format() const noexcept override { return format_; }

private:
    void on_thread_start() noexcept override { com_ok_ = SUCCEEDED(com_hr_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    void on_thread_stop() noexcept override {
        close_device();
        if (com_ok_) ::CoUninitialize();
        com_ok_ = false;
    }

    Status open_device(std::uint32_t sample_rate, std::uint32_t channels) noexcept override {
        if (!com_ok_ && com_hr_ != RPC_E_CHANGED_MODE) return {MIXIT_E_PLATFORM, "COM initialisation failed"};
        channels_ = channels;
        ComRef<IMMDeviceEnumerator> e;
        if (Status st = enumerator(e); !st.is_ok()) return st;
        ComRef<IMMDevice> device;
        if (Status st = find_device(e.get(), DeviceDirection::Capture, id_, device); !st.is_ok()) return st;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.put()));
        if (FAILED(hr)) return {MIXIT_E_PLATFORM, "IAudioClient activation failed (" + hr_text(hr) + ")"};
        WAVEFORMATEXTENSIBLE fmt = float_format(sample_rate, channels);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 2'000'000 /* 200 ms */, 0, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) return {MIXIT_E_NOT_SUPPORTED, "the device refused the stream format (" + hr_text(hr) + ")"};
        hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.put()));
        if (FAILED(hr)) return {MIXIT_E_PLATFORM, "IAudioCaptureClient " + hr_text(hr)};
        if (FAILED(hr = client_->Start())) return {MIXIT_E_PLATFORM, "IAudioClient::Start " + hr_text(hr)};
        return Status::ok();
    }

    bool pump(SampleRing& ring) noexcept override {
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture_->GetNextPacketSize(&packet))) return false;
            if (packet == 0) return true;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return false;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                silence_.assign(static_cast<std::size_t>(frames) * channels_, 0.0f);
                ring.write(silence_.data(), frames);
            } else {
                ring.write(reinterpret_cast<const float*>(data), frames);
            }
            capture_->ReleaseBuffer(frames);
        }
    }

    void close_device() noexcept override {
        if (client_) client_->Stop();
        capture_.reset();
        client_.reset();
    }
    void log_failure(const Status& st) noexcept override {
        logger_.logf(MIXIT_LOG_ERROR, "audio", "{}: {} — retrying every second", format_.description, st.message());
    }
    void log_restored() noexcept override {
        logger_.logf(MIXIT_LOG_INFO, "audio", "{}: device available again", format_.description);
    }

    std::string id_;
    Logger& logger_;
    media::SourceFormat format_;
    bool com_ok_ = false;
    HRESULT com_hr_ = E_FAIL;
    ComRef<IAudioClient> client_;
    ComRef<IAudioCaptureClient> capture_;
    std::uint32_t channels_ = 2;
    std::vector<float> silence_;
};

}  // namespace

Status enumerate_audio_devices(DeviceDirection direction, Logger& logger, std::vector<AudioDeviceInfo>& out) {
    out.clear();
    Status st = on_mta_thread([&]() -> Status {
        ComRef<IMMDeviceEnumerator> e;
        if (Status s = enumerator(e); !s.is_ok()) return s;
        const EDataFlow flow = direction == DeviceDirection::Render ? eRender : eCapture;
        std::string default_id;
        ComRef<IMMDevice> def;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole, def.put()))) default_id = device_id(def.get());
        ComRef<IMMDeviceCollection> devices;
        HRESULT hr = e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put());
        if (FAILED(hr)) return {MIXIT_E_PLATFORM, "EnumAudioEndpoints " + hr_text(hr)};
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComRef<IMMDevice> d;
            if (FAILED(devices->Item(i, d.put()))) continue;
            AudioDeviceInfo info;
            info.id = device_id(d.get());
            if (info.id.empty()) continue;
            info.name = friendly_name(d.get());
            info.is_default = info.id == default_id;
            ComRef<IAudioClient> client;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put())))) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
                    info.channels = mix->nChannels;
                    info.sample_rate = mix->nSamplesPerSec;
                    ::CoTaskMemFree(mix);
                }
            }
            out.push_back(std::move(info));
        }
        return Status::ok();
    });
    if (!st.is_ok()) logger.logf(MIXIT_LOG_WARNING, "audio", "audio device enumeration failed: {}", st.message());
    return st;
}

Status open_audio_output(const AudioOutputConfig& config, SampleRing& ring, Logger& logger,
                         std::unique_ptr<AudioOutput>& out) {
    NullDeviceSpec null_spec;
    if (parse_null_device(config.device_id, null_spec)) return open_null_output(config, null_spec, ring, out);
    // Check the device exists now so a typo fails the call instead of retrying forever.
    Status st = on_mta_thread([&]() -> Status {
        ComRef<IMMDeviceEnumerator> e;
        if (Status s = enumerator(e); !s.is_ok()) return s;
        ComRef<IMMDevice> device;
        return find_device(e.get(), DeviceDirection::Render, config.device_id, device);
    });
    if (!st.is_ok()) return st;
    try {
        out = std::make_unique<WasapiOutput>(config, ring, logger);
    } catch (const std::system_error& e) {
        return {MIXIT_E_PLATFORM, std::string("cannot start the output thread: ") + e.what()};
    }
    return Status::ok();
}

Status open_audio_capture_source(const std::string& device_id, Logger& logger, std::unique_ptr<media::MediaSource>& out) {
    NullDeviceSpec null_spec;
    if (parse_null_device(device_id, null_spec)) return open_null_capture_source(null_spec, out);
    std::string name;
    Status st = on_mta_thread([&]() -> Status {
        ComRef<IMMDeviceEnumerator> e;
        if (Status s = enumerator(e); !s.is_ok()) return s;
        ComRef<IMMDevice> device;
        if (Status s = find_device(e.get(), DeviceDirection::Capture, device_id, device); !s.is_ok()) return s;
        name = friendly_name(device.get());
        return Status::ok();
    });
    if (!st.is_ok()) return st;
    out = std::make_unique<WasapiCapture>(device_id, name.empty() ? std::string("default") : name, logger);
    return Status::ok();
}

}  // namespace mixit::audio

#else  // !_WIN32: development hosts have only the null test device.

namespace mixit::audio {

Status enumerate_audio_devices(DeviceDirection, Logger&, std::vector<AudioDeviceInfo>& out) {
    out.clear();
    return {MIXIT_E_NOT_SUPPORTED, "audio devices are implemented for Windows (WASAPI) only"};
}

Status open_audio_output(const AudioOutputConfig& config, SampleRing& ring, Logger&, std::unique_ptr<AudioOutput>& out) {
    NullDeviceSpec spec;
    if (parse_null_device(config.device_id, spec)) return open_null_output(config, spec, ring, out);
    return {MIXIT_E_NOT_SUPPORTED, "audio output is implemented for Windows (WASAPI) only"};
}

Status open_audio_capture_source(const std::string& device_id, Logger&, std::unique_ptr<media::MediaSource>& out) {
    NullDeviceSpec spec;
    if (parse_null_device(device_id, spec)) return open_null_capture_source(spec, out);
    return {MIXIT_E_NOT_SUPPORTED, "audio capture is implemented for Windows (WASAPI) only"};
}

}  // namespace mixit::audio

#endif
