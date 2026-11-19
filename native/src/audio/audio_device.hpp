// Audio devices (M4): enumeration, monitor/headphone output, audio capture.
//
// Backends:
//   * WASAPI shared mode (Windows; audio_device_wasapi.cpp) — EXPERIMENTAL
//     until verified on hardware. Streams are opened as float32 at the
//     session's rate and channel count; Windows converts to the device format
//     (AUTOCONVERTPCM), and a DriftResampler bridges device and master clocks.
//   * Null device (all platforms; id "mixit:null" or "mixit:null:<ppm>") — a
//     development/test substitute paced by the monotonic clock with an
//     optional clock offset in ppm. Not listed by enumeration.
//
// Device ids are opaque strings (WASAPI endpoint ids); an empty id selects
// the system default device of that direction.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/drift_resampler.hpp"
#include "core/status.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {
class MediaSource;
}

namespace mixit::audio {

enum class DeviceDirection : std::int32_t { Render = 0, Capture = 1 };

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    std::uint32_t channels = 0;     // device mix format
    std::uint32_t sample_rate = 0;
};

Status enumerate_audio_devices(DeviceDirection direction, Logger& logger, std::vector<AudioDeviceInfo>& out);

enum class OutputState : std::int32_t { Off = 0, Running = 1, Retrying = 2 };

struct AudioOutputStats {
    OutputState state = OutputState::Off;
    std::uint64_t underruns = 0;      // device asked for audio the mixer had not delivered
    std::uint64_t device_errors = 0;  // open/stream failures (device unplugged, format refused)
    double ratio = 1.0;               // drift-compensation playback ratio
    std::uint32_t buffered_frames = 0;
    std::string device_name;
};

// Plays a SampleRing (filled by the mixer at the master clock rate) on a
// device. Owns one thread; stops it in the destructor.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    [[nodiscard]] virtual AudioOutputStats stats() const = 0;
};

struct AudioOutputConfig {
    std::string device_id;          // "" = default render device
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t latency_ms = 40;  // ring fill target
};

Status open_audio_output(const AudioOutputConfig& config, SampleRing& ring, Logger& logger,
                         std::unique_ptr<AudioOutput>& out);

// An audio-only input from a capture device (microphone, line in, the audio
// of a USB/HDMI capture device). Video: none.
Status open_audio_capture_source(const std::string& device_id, Logger& logger,
                                 std::unique_ptr<media::MediaSource>& out);

// ---- shared by backends ------------------------------------------------------

struct NullDeviceSpec {
    double ppm = 0.0;
};
// True if `id` names the null test device ("mixit:null[:ppm]").
bool parse_null_device(const std::string& id, NullDeviceSpec& spec);

Status open_null_output(const AudioOutputConfig& config, const NullDeviceSpec& spec, SampleRing& ring,
                        std::unique_ptr<AudioOutput>& out);
Status open_null_capture_source(const NullDeviceSpec& spec, std::unique_ptr<media::MediaSource>& out);

}  // namespace mixit::audio
