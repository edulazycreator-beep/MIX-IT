// Shared loop for audio capture inputs: the device side fills a SampleRing
// at the device clock; this loop emits one block per 10 ms on the session
// sample timeline (master clock), pulling through a DriftResampler. Device
// loss yields silence blocks (the timeline never stalls) and a retry every
// second.
#pragma once

#include <atomic>
#include <cstdint>

#include "audio/drift_resampler.hpp"
#include "core/status.hpp"
#include "media/media_source.hpp"

namespace mixit::audio {

class CapturePacer : public media::MediaSource {
public:
    void run(media::SourceIo& io) noexcept final;
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

protected:
    static constexpr std::int64_t kLatencyNs = 20'000'000;  // after a block is due

    // Called on the capture thread only (start/stop bracket the thread's life,
    // e.g. COM apartment membership).
    virtual void on_thread_start() noexcept {}
    virtual void on_thread_stop() noexcept {}
    virtual Status open_device(std::uint32_t sample_rate, std::uint32_t channels) noexcept = 0;
    virtual bool pump(SampleRing& ring) noexcept = 0;  // drain the device into the ring; false = device failed
    virtual void close_device() noexcept = 0;
    virtual void log_failure(const Status& status) noexcept = 0;
    virtual void log_restored() noexcept = 0;

private:
    std::atomic<std::uint64_t> underruns_{0};
};

}  // namespace mixit::audio
