// Media Foundation video capture (Windows). Public, documented APIs only:
// MFEnumDeviceSources, MFCreateDeviceSource, MFCreateSourceReaderFromMediaSource,
// IMFSourceReader (synchronous mode) with its built-in video processor
// converting the device format (NV12, YUY2, MJPG, ...) to RGB32.
//
// COM/MF are used only on threads this file owns or initialises itself
// (multithreaded apartment): enumeration and mode probing run on a short-lived
// helper thread, capture runs on the input's thread.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <thread>

#include "logging/logger.hpp"
#include "media/capture_source.hpp"
#include "platform/com_ref.hpp"
#include "platform/win_strings.hpp"
#include "util/clock.hpp"

namespace mixit::media {
namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Media Foundation entry points, resolved at run time: Windows "N" editions
// ship without Media Foundation, and a load-time import would stop the whole
// engine DLL from loading there. Without MF, capture reports NOT_SUPPORTED.
struct MfApi {
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* EnumDeviceSources)(IMFAttributes*, IMFActivate***, UINT32*) = nullptr;
    HRESULT(WINAPI* CreateDeviceSource)(IMFAttributes*, IMFMediaSource**) = nullptr;
    HRESULT(WINAPI* CreateSourceReaderFromMediaSource)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**) = nullptr;
    bool ok = false;
};

template <typename Fn>
void resolve(HMODULE m, const char* name, Fn& fn) {
    fn = m == nullptr ? nullptr : reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
}

const MfApi& mf_api() {
    static const MfApi api = [] {
        MfApi a;
        // Loaded for the life of the process (never freed: MF threads may outlive callers).
        HMODULE plat = ::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE mf = ::LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE rw = ::LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        resolve(plat, "MFStartup", a.Startup);
        resolve(plat, "MFShutdown", a.Shutdown);
        resolve(plat, "MFCreateAttributes", a.CreateAttributes);
        resolve(plat, "MFCreateMediaType", a.CreateMediaType);
        resolve(mf, "MFEnumDeviceSources", a.EnumDeviceSources);
        resolve(mf, "MFCreateDeviceSource", a.CreateDeviceSource);
        resolve(rw, "MFCreateSourceReaderFromMediaSource", a.CreateSourceReaderFromMediaSource);
        a.ok = a.Startup && a.Shutdown && a.CreateAttributes && a.CreateMediaType && a.EnumDeviceSources &&
               a.CreateDeviceSource && a.CreateSourceReaderFromMediaSource;
        return a;
    }();
    return api;
}

std::string hr_text(HRESULT hr) { return std::format("0x{:08X}", static_cast<unsigned>(hr)); }

// COM (MTA) + Media Foundation for the current thread, undone on scope exit.
class MfThreadScope {
public:
    MfThreadScope() {
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ = SUCCEEDED(co);  // S_FALSE = already initialised: still balanced by CoUninitialize
        hr_ = co == RPC_E_CHANGED_MODE ? S_OK : co;
        if (SUCCEEDED(hr_)) {
            hr_ = mf_api().ok ? mf_api().Startup(MF_VERSION, MFSTARTUP_NOSOCKET) : E_NOTIMPL;
            mf_ = SUCCEEDED(hr_);
        }
    }
    ~MfThreadScope() {
        if (mf_) mf_api().Shutdown();
        if (com_) ::CoUninitialize();
    }
    MfThreadScope(const MfThreadScope&) = delete;
    MfThreadScope& operator=(const MfThreadScope&) = delete;
    [[nodiscard]] HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_ = E_FAIL;
    bool com_ = false;
    bool mf_ = false;
};

// Runs `fn` on a fresh MTA thread (callers may be STA UI threads).
Status run_on_mf_thread(const std::function<Status()>& fn) {
    if (!mf_api().ok) return {MIXIT_E_NOT_SUPPORTED, "Media Foundation is not installed (Windows N: add the Media Feature Pack)"};
    Status result{MIXIT_E_INTERNAL, "capture helper thread did not run"};
    try {
        std::thread t([&] {
            MfThreadScope mf;
            if (FAILED(mf.result())) {
                result = {MIXIT_E_PLATFORM, "Media Foundation start-up failed (" + hr_text(mf.result()) + ")"};
                return;
            }
            try {
                result = fn();
            } catch (const std::exception& e) {
                result = {MIXIT_E_INTERNAL, e.what()};
            }
        });
        t.join();
    } catch (const std::system_error& e) {
        return {MIXIT_E_PLATFORM, std::string("cannot start capture helper thread: ") + e.what()};
    }
    return result;
}

std::string allocated_string(IMFActivate* a, const GUID& key) {
    WCHAR* text = nullptr;
    UINT32 length = 0;
    if (FAILED(a->GetAllocatedString(key, &text, &length)) || text == nullptr) return {};
    std::string s = os::wide_to_utf8(std::wstring_view(text, length));
    ::CoTaskMemFree(text);
    return s;
}

Status enumerate_on_mf_thread(std::vector<CaptureDeviceInfo>& out) {
    ComRef<IMFAttributes> attributes;
    if (HRESULT hr = mf_api().CreateAttributes(attributes.put(), 1); FAILED(hr)) return {MIXIT_E_PLATFORM, "MFCreateAttributes " + hr_text(hr)};
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (HRESULT hr = mf_api().EnumDeviceSources(attributes.get(), &devices, &count); FAILED(hr)) {
        return {MIXIT_E_PLATFORM, "MFEnumDeviceSources " + hr_text(hr)};
    }
    for (UINT32 i = 0; i < count; ++i) {
        CaptureDeviceInfo d;
        d.name = allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        d.id = allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        devices[i]->Release();
        if (!d.id.empty()) out.push_back(std::move(d));
    }
    ::CoTaskMemFree(static_cast<void*>(devices));
    return Status::ok();
}

struct Mode {
    DWORD index = 0;
    std::uint32_t width = 0, height = 0;
    Rate rate{0, 1};
    GUID subtype{};
};

void unpack(UINT64 v, std::uint32_t& hi, std::uint32_t& lo) {
    hi = static_cast<std::uint32_t>(v >> 32);
    lo = static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
}

int subtype_preference(const GUID& g) {
    if (g == MFVideoFormat_NV12) return 4;
    if (g == MFVideoFormat_YUY2) return 3;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return 2;
    if (g == MFVideoFormat_MJPG) return 1;  // decoded on the CPU by the reader
    return 0;
}

double fps(const Rate& r) { return r.den == 0 ? 0.0 : static_cast<double>(r.num) / r.den; }

// Chooses a native mode: the requested size (and closest rate) if given,
// otherwise the largest size up to 1080p at the highest rate up to 60 fps.
bool choose_mode(IMFSourceReader* reader, const CaptureOptions& o, Mode& chosen, std::string& available) {
    bool found = false;
    for (DWORD i = 0;; ++i) {
        ComRef<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(kVideoStream, i, type.put()))) break;
        Mode m;
        m.index = i;
        UINT64 packed = 0;
        if (FAILED(type->GetUINT64(MF_MT_FRAME_SIZE, &packed))) continue;
        unpack(packed, m.width, m.height);
        if (SUCCEEDED(type->GetUINT64(MF_MT_FRAME_RATE, &packed))) unpack(packed, m.rate.num, m.rate.den);
        type->GetGUID(MF_MT_SUBTYPE, &m.subtype);
        if (m.width == 0 || m.height == 0 || m.width % 2 != 0 || m.height % 2 != 0 || fps(m.rate) <= 0) continue;
        if (available.size() < 900) available += std::format("{}x{}@{:.2f} ", m.width, m.height, fps(m.rate));

        if (o.width != 0 && (m.width != o.width || m.height != o.height)) continue;
        if (!found) {
            chosen = m;
            found = true;
            continue;
        }
        auto better = [&](const Mode& a, const Mode& b) {  // is a better than b?
            const bool a_fits = std::uint64_t{a.width} * a.height <= 1920ull * 1080;
            const bool b_fits = std::uint64_t{b.width} * b.height <= 1920ull * 1080;
            if (o.width == 0 && a_fits != b_fits) return a_fits;
            if (o.width == 0 && std::uint64_t{a.width} * a.height != std::uint64_t{b.width} * b.height) {
                return std::uint64_t{a.width} * a.height > std::uint64_t{b.width} * b.height;
            }
            const double target = o.frame_rate.num != 0 ? fps(o.frame_rate) : 60.0;
            const double da = std::abs(fps(a.rate) - target) + (fps(a.rate) > target + 0.01 ? 1000.0 : 0.0);
            const double db = std::abs(fps(b.rate) - target) + (fps(b.rate) > target + 0.01 ? 1000.0 : 0.0);
            if (std::abs(da - db) > 0.01) return da < db;
            return subtype_preference(a.subtype) > subtype_preference(b.subtype);
        };
        if (better(m, chosen)) chosen = m;
    }
    return found;
}

// An opened device: media source + synchronous source reader producing RGB32.
struct OpenDevice {
    ComRef<IMFMediaSource> source;
    ComRef<IMFSourceReader> reader;
    LONG default_stride = 0;  // bytes; negative = bottom-up
    ~OpenDevice() {
        reader.reset();
        if (source) source->Shutdown();
    }
};

Status open_device(const std::string& id, const CaptureOptions& options, Mode* mode_inout, OpenDevice& dev,
                   std::string* available = nullptr) {
    ComRef<IMFAttributes> a;
    if (HRESULT hr = mf_api().CreateAttributes(a.put(), 2); FAILED(hr)) return {MIXIT_E_PLATFORM, "MFCreateAttributes " + hr_text(hr)};
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    const std::wstring wide = os::utf8_to_wide(id);
    a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wide.c_str());
    if (HRESULT hr = mf_api().CreateDeviceSource(a.get(), dev.source.put()); FAILED(hr)) {
        return {MIXIT_E_NOT_FOUND, "capture device unavailable (" + hr_text(hr) + ")"};
    }
    ComRef<IMFAttributes> ra;
    if (HRESULT hr = mf_api().CreateAttributes(ra.put(), 1); FAILED(hr)) return {MIXIT_E_PLATFORM, "MFCreateAttributes " + hr_text(hr)};
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (HRESULT hr = mf_api().CreateSourceReaderFromMediaSource(dev.source.get(), ra.get(), dev.reader.put()); FAILED(hr)) {
        return {MIXIT_E_PLATFORM, "MFCreateSourceReaderFromMediaSource " + hr_text(hr)};
    }
    dev.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    dev.reader->SetStreamSelection(kVideoStream, TRUE);

    Mode mode = mode_inout != nullptr ? *mode_inout : Mode{};
    if (mode.width == 0) {
        std::string list;
        if (!choose_mode(dev.reader.get(), options, mode, list)) {
            return {MIXIT_E_NOT_SUPPORTED, "no matching capture mode; device offers: " + (list.empty() ? "nothing usable" : list)};
        }
        if (available != nullptr) *available = list;
    }
    ComRef<IMFMediaType> native;
    if (HRESULT hr = dev.reader->GetNativeMediaType(kVideoStream, mode.index, native.put()); FAILED(hr)) {
        return {MIXIT_E_PLATFORM, "GetNativeMediaType " + hr_text(hr)};
    }
    if (HRESULT hr = dev.reader->SetCurrentMediaType(kVideoStream, nullptr, native.get()); FAILED(hr)) {
        return {MIXIT_E_PLATFORM, "selecting the capture mode failed (" + hr_text(hr) + ")"};
    }
    ComRef<IMFMediaType> rgb;
    if (HRESULT hr = mf_api().CreateMediaType(rgb.put()); FAILED(hr)) return {MIXIT_E_PLATFORM, "MFCreateMediaType " + hr_text(hr)};
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (HRESULT hr = dev.reader->SetCurrentMediaType(kVideoStream, nullptr, rgb.get()); FAILED(hr)) {
        return {MIXIT_E_NOT_SUPPORTED, "the device format cannot be converted to RGB32 (" + hr_text(hr) + ")"};
    }
    ComRef<IMFMediaType> current;
    if (SUCCEEDED(dev.reader->GetCurrentMediaType(kVideoStream, current.put()))) {
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) dev.default_stride = static_cast<LONG>(stride);
        UINT64 packed = 0;
        if (SUCCEEDED(current->GetUINT64(MF_MT_FRAME_SIZE, &packed))) unpack(packed, mode.width, mode.height);
    }
    // Uncompressed RGB without an explicit stride is bottom-up in Media Foundation.
    if (dev.default_stride == 0) dev.default_stride = -static_cast<LONG>(mode.width * 4);
    if (mode_inout != nullptr) *mode_inout = mode;
    return Status::ok();
}

class CaptureSource final : public MediaSource {
public:
    CaptureSource(CaptureOptions options, const Mode& mode, std::string name, Logger& logger)
        : options_(std::move(options)), mode_(mode), logger_(logger) {
        format_.has_video = true;
        format_.has_audio = false;
        format_.width = mode.width;
        format_.height = mode.height;
        format_.nominal_frame_rate = mode.rate;
        format_.description = std::format("capture '{}' {}x{} @ {:.2f} fps (EXPERIMENTAL)", name, mode.width,
                                          mode.height, fps(mode.rate));
    }

    [[nodiscard]] const SourceFormat& format() const noexcept override { return format_; }

    void run(SourceIo& io) noexcept override {
        MfThreadScope mf;
        if (FAILED(mf.result())) {
            logger_.logf(MIXIT_LOG_ERROR, "capture", "Media Foundation start-up failed ({})", hr_text(mf.result()));
            return;
        }
        std::int64_t sequence = 0;
        bool reported = false;
        while (!io.stop_requested()) {
            OpenDevice dev;
            Mode mode = mode_;
            Status st;
            try {
                st = open_device(options_.device_id, options_, &mode, dev);
            } catch (...) {
                st = {MIXIT_E_INTERNAL, "unexpected exception opening the capture device"};
            }
            if (st.is_ok() && (mode.width != mode_.width || mode.height != mode_.height)) {
                st = {MIXIT_E_NOT_SUPPORTED, std::format("device now delivers {}x{}", mode.width, mode.height)};
            }
            if (!st.is_ok()) {
                if (!reported) {
                    logger_.logf(MIXIT_LOG_ERROR, "capture", "{}: {} — retrying every second", format_.description, st.message());
                    reported = true;
                }
                io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
                io.sleep_until(static_cast<std::int64_t>(monotonic_ns()) + 1'000'000'000);
                continue;
            }
            if (reported) logger_.logf(MIXIT_LOG_INFO, "capture", "{}: device available again", format_.description);
            reported = false;
            if (!capture(io, dev, sequence)) {
                io.sleep_until(static_cast<std::int64_t>(monotonic_ns()) + 1'000'000'000);  // device error: back off
            }
        }
    }

private:
    // Reads frames until stop (true) or a device error / format change (false).
    bool capture(SourceIo& io, OpenDevice& dev, std::int64_t& sequence) noexcept {
        while (!io.stop_requested()) {
            DWORD stream = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComRef<IMFSample> sample;
            const HRESULT hr = dev.reader->ReadSample(kVideoStream, 0, &stream, &flags, &timestamp, sample.put());
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
                logger_.logf(MIXIT_LOG_ERROR, "capture", "{}: capture stopped ({}, flags {:#x})", format_.description,
                             hr_text(hr), static_cast<unsigned>(flags));
                io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                logger_.logf(MIXIT_LOG_WARNING, "capture", "{}: device changed its format; reopening", format_.description);
                return false;
            }
            if (!sample) continue;  // stream tick / gap
            const auto arrival = static_cast<std::int64_t>(monotonic_ns());
            VideoRef frame = io.acquire_video();
            if (!frame) continue;
            if (!copy_sample(sample.get(), dev.default_stride, *frame)) {
                io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            frame->pts_ns = arrival;
            frame->sequence = sequence++;
            frame->flags = 0;
            io.push_video(std::move(frame));
        }
        return true;
    }

    bool copy_sample(IMFSample* sample, LONG default_stride, VideoFrame& dst) noexcept {
        ComRef<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) return false;
        const std::size_t row = static_cast<std::size_t>(dst.width) * 4;
        ComRef<IMF2DBuffer> buffer2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        bool locked2d = false;
        BYTE* raw = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(buffer2d.put()))) &&
            SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
            locked2d = true;
        } else if (SUCCEEDED(buffer->Lock(&raw, nullptr, &length))) {
            pitch = default_stride;
            const std::size_t needed = static_cast<std::size_t>(std::abs(pitch)) * dst.height;
            if (length < needed) {
                buffer->Unlock();
                return false;
            }
            scan0 = pitch < 0 ? raw + static_cast<std::size_t>(-pitch) * (dst.height - 1) : raw;
        } else {
            return false;
        }
        for (std::uint32_t y = 0; y < dst.height; ++y) {
            const BYTE* src = scan0 + static_cast<std::ptrdiff_t>(pitch) * static_cast<std::ptrdiff_t>(y);
            std::uint8_t* out = dst.data + static_cast<std::size_t>(y) * dst.stride;
            std::memcpy(out, src, row);
            for (std::size_t x = 3; x < row; x += 4) out[x] = 255;  // RGB32: the fourth byte is undefined
        }
        if (locked2d) {
            buffer2d->Unlock2D();
        } else {
            buffer->Unlock();
        }
        return true;
    }

    CaptureOptions options_;
    Mode mode_;
    Logger& logger_;
    SourceFormat format_;
};

}  // namespace

Status enumerate_capture_devices(Logger& logger, std::vector<CaptureDeviceInfo>& out) {
    out.clear();
    Status st = run_on_mf_thread([&] { return enumerate_on_mf_thread(out); });
    if (!st.is_ok()) logger.logf(MIXIT_LOG_WARNING, "capture", "capture device enumeration failed: {}", st.message());
    return st;
}

Status open_capture_source(const CaptureOptions& options, Logger& logger, std::unique_ptr<MediaSource>& out) {
    if (options.device_id.empty()) return {MIXIT_E_INVALID_ARGUMENT, "capture device id is empty"};
    if ((options.width == 0) != (options.height == 0)) return {MIXIT_E_INVALID_ARGUMENT, "set both width and height, or neither"};
    std::vector<CaptureDeviceInfo> devices;
    if (Status st = enumerate_capture_devices(logger, devices); !st.is_ok()) return st;
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const CaptureDeviceInfo& d) { return d.id == options.device_id; });
    if (it == devices.end()) return {MIXIT_E_NOT_FOUND, "no capture device with that id is connected"};

    Mode mode;
    std::string available;
    Status st = run_on_mf_thread([&] {
        OpenDevice dev;  // probe only; capture reopens the device on its own thread
        return open_device(options.device_id, options, &mode, dev, &available);
    });
    if (!st.is_ok()) return st;
    logger.logf(MIXIT_LOG_INFO, "capture", "'{}': using {}x{} @ {:.2f} fps (modes: {})", it->name, mode.width, mode.height,
                fps(mode.rate), available);
    try {
        out = std::make_unique<CaptureSource>(options, mode, it->name, logger);
    } catch (const std::bad_alloc&) {
        return {MIXIT_E_OUT_OF_MEMORY, "capture source"};
    }
    return Status::ok();
}

}  // namespace mixit::media

#else  // !_WIN32

#include "media/capture_source.hpp"

namespace mixit::media {

Status enumerate_capture_devices(Logger&, std::vector<CaptureDeviceInfo>& out) {
    out.clear();
    return {MIXIT_E_NOT_SUPPORTED, "video capture is implemented for Windows (Media Foundation) only"};
}

Status open_capture_source(const CaptureOptions&, Logger&, std::unique_ptr<MediaSource>&) {
    return {MIXIT_E_NOT_SUPPORTED, "video capture is implemented for Windows (Media Foundation) only"};
}

}  // namespace mixit::media

#endif
