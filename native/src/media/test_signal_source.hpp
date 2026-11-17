// MediaSource producing the deterministic test signal (see test_signal.hpp),
// paced in real time like a capture device.
#pragma once

#include "media/media_source.hpp"
#include "media/test_signal.hpp"

namespace mixit::media {

class TestSignalSource final : public MediaSource {
public:
    explicit TestSignalSource(const TestSignalConfig& config);

    [[nodiscard]] const SourceFormat& format() const noexcept override { return format_; }
    void run(SourceIo& io) noexcept override;
    [[nodiscard]] const TestSignal& signal() const noexcept { return signal_; }

private:
    TestSignal signal_;
    SourceFormat format_;
};

}  // namespace mixit::media
