// VST3 plug-in host (M4, EXPERIMENTAL). Original host implementation on the
// Steinberg VST3 interface headers (MIT); no SDK hosting helpers.
//
// Scope:
//   * Scan: load .vst3 bundles, list "Audio Module Class" effects.
//   * Insert effects on mixer strips: stereo or mono in/out, 32-bit float,
//     real-time process mode, fixed 10 ms blocks; parameters (normalized
//     values, sample-accurate at block start), bypass, component +
//     controller state.
//   * Not yet: plug-in editors (IPlugView), sidechains, MIDI/events,
//     latency compensation, host messages (IHostApplication::createInstance
//     returns kNotImplemented).
//
// Threading follows the VST3 rules: lifecycle, state and controller calls on
// a control thread; process() on the audio thread only.
//
// Crash isolation: plug-ins run in the engine process. A faulty plug-in can
// take the engine down; an out-of-process plug-in host is planned (ADR-0008).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"

namespace mixit::audio {

struct Vst3PluginInfo {
    std::string path;      // bundle (or single-file .vst3) path
    std::string class_id;  // 32 hex digits
    std::string name;
    std::string vendor;
    std::string version;
    std::string subcategories;  // e.g. "Fx|EQ"
};

struct Vst3ParameterInfo {
    std::uint32_t id = 0;
    std::string title;
    std::string units;
    std::int32_t step_count = 0;      // 0 = continuous
    double default_normalized = 0.0;
    bool read_only = false;
    bool is_bypass = false;
};

// Scans one bundle/file or every *.vst3 under a folder (recursively, max depth 3).
// Plug-ins that fail to load are skipped and reported in `errors`.
Status vst3_scan(const std::string& path, std::vector<Vst3PluginInfo>& out, std::vector<std::string>& errors);

// Default plug-in folders of the platform (existing ones only).
std::vector<std::string> vst3_default_folders();

class Vst3Module;

// One plug-in instance, ready to process (created active).
class Vst3Effect {
public:
    // Loads the plug-in class and prepares it for `sample_rate` / `channels`
    // (1 or 2) / `max_block` frames per process call.
    static Status create(const std::string& path, const std::string& class_id, double sample_rate,
                         std::uint32_t channels, std::uint32_t max_block, std::unique_ptr<Vst3Effect>& out);
    ~Vst3Effect();
    Vst3Effect(const Vst3Effect&) = delete;
    Vst3Effect& operator=(const Vst3Effect&) = delete;

    // ---- audio thread ----
    // Processes interleaved `frames` (<= max_block) in place; applies queued
    // parameter changes at the start of the block. No allocation, no locks.
    // frames == 0 only delivers pending parameter changes (the session does
    // this on the control thread while its audio thread is not running).
    void process(float* interleaved, std::uint32_t frames) noexcept;

    // ---- control thread ----
    [[nodiscard]] const Vst3PluginInfo& info() const noexcept;
    [[nodiscard]] const std::vector<Vst3ParameterInfo>& parameters() const noexcept;
    Status set_parameter(std::uint32_t id, double normalized);  // queued for the audio thread
    Status get_parameter(std::uint32_t id, double& normalized) const;
    std::string parameter_text(std::uint32_t id, double normalized) const;
    void set_bypass(bool bypass) noexcept { bypass_.store(bypass, std::memory_order_relaxed); }
    [[nodiscard]] bool bypass() const noexcept { return bypass_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t latency_samples() const noexcept;
    // Component + controller state as one opaque blob, and back.
    Status get_state(std::vector<std::uint8_t>& out) const;
    Status set_state(const std::uint8_t* data, std::size_t size);
    [[nodiscard]] std::uint64_t process_errors() const noexcept { return process_errors_.load(std::memory_order_relaxed); }

    struct Impl;

private:
    Vst3Effect();
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> bypass_{false};
    std::atomic<std::uint64_t> process_errors_{0};
};

}  // namespace mixit::audio
