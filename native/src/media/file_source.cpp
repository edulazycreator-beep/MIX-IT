#include "media/file_source.hpp"

#include "logging/logger.hpp"
#include "media/ffmpeg_library.hpp"

#if MIXIT_HAVE_FFMPEG

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace mixit::media {
namespace {

std::string av_error_text(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Fixed-capacity FIFO of pooled refs (no allocation on the media thread).
template <typename T, std::size_t N>
class Ring {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool full() const noexcept { return count_ == N; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    T& front() noexcept { return items_[head_]; }
    void push(T&& v) noexcept {
        items_[(head_ + count_) % N] = std::move(v);
        ++count_;
    }
    T pop() noexcept {
        T v = std::move(items_[head_]);
        head_ = (head_ + 1) % N;
        --count_;
        return v;
    }
    void clear() noexcept {
        while (!empty()) (void)pop();
    }

private:
    std::array<T, N> items_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

struct AvFree {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
    void operator()(SwsContext* p) const noexcept { sws_freeContext(p); }
    void operator()(SwrContext* p) const noexcept { swr_free(&p); }
};
template <typename T>
using AvPtr = std::unique_ptr<T, AvFree>;

}  // namespace

class FileSource final : public MediaSource {
public:
    FileSource(std::string path, FileSourceOptions options, Logger& logger)
        : path_(std::move(path)), options_(options), logger_(logger) {}

    Status open();
    [[nodiscard]] const SourceFormat& format() const noexcept override { return format_; }
    void run(SourceIo& io) noexcept override;

private:
    // Video read-ahead stays at 3 frames (ring capacity 4) so that, with the
    // input queue (4), frame-sync refs (2) and program refs (3), the session's
    // 12-frame input pool is never exhausted by read-ahead alone.
    static constexpr std::size_t kVideoAhead = 4;
    static constexpr std::size_t kAudioAhead = 24;

    std::int64_t media_ns(std::int64_t ts, AVRational tb) const noexcept {
        if (ts == AV_NOPTS_VALUE) return std::numeric_limits<std::int64_t>::min();
        const auto ns = muldiv_i64(ts, static_cast<std::int64_t>(tb.num) * kNanosPerSecond, tb.den, Rounding::Nearest);
        return ns.value_or(0) - start_ns_;
    }

    bool restart(SourceIo& io) noexcept;
    bool decode_more(SourceIo& io) noexcept;  // false at end of file (decoders drained)
    void handle_video_frame(SourceIo& io) noexcept;
    void handle_audio_frame(SourceIo& io) noexcept;
    void convert_audio(SourceIo& io, const std::uint8_t** in, int in_count) noexcept;
    void emit_silence_until(SourceIo& io, std::int64_t sample_index) noexcept;
    void finish_block(SourceIo& io) noexcept;
    void drain_decoder(SourceIo& io, AVCodecContext* dec, bool video) noexcept;
    // Paces and pushes the earliest buffered item. False if stopped or empty.
    bool push_earliest(SourceIo& io) noexcept;

    std::string path_;
    FileSourceOptions options_;
    Logger& logger_;
    SourceFormat format_;

    AvPtr<AVFormatContext> fmt_;
    AvPtr<AVCodecContext> vdec_;
    AvPtr<AVCodecContext> adec_;
    AvPtr<AVFrame> frame_;
    AvPtr<AVPacket> packet_;
    AvPtr<SwsContext> sws_;
    AvPtr<SwrContext> swr_;
    int vstream_ = -1;
    int astream_ = -1;
    std::int64_t start_ns_ = 0;
    std::int64_t nominal_frame_ns_ = 33'333'333;

    // Per-run state (run thread only).
    Ring<VideoRef, kVideoAhead> video_ring_;
    Ring<AudioRef, kAudioAhead> audio_ring_;
    AudioRef block_;
    std::uint32_t block_filled_ = 0;
    std::int64_t next_sample_ = 0;        // absolute output sample index of the next sample produced
    std::int64_t loop_start_sample_ = 0;
    std::int64_t loop_offset_ns_ = 0;
    std::int64_t loop_video_end_ns_ = 0;  // media time end of the last frame in this loop
    std::int64_t frame_counter_ = 0;
    std::int64_t last_video_media_ns_ = 0;
    bool eof_ = false;
};

Status FileSource::open() {
    const FfmpegInfo& ff = FfmpegLibrary::info();
    if (!ff.usable()) return {MIXIT_E_NOT_SUPPORTED, "file inputs need a usable FFmpeg (absent or rejected by the licence policy)"};

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);  // never network/device URLs from a media file
    AVFormatContext* raw = nullptr;
    const int err = avformat_open_input(&raw, path_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        const mixit_result code = err == AVERROR(ENOENT) ? MIXIT_E_NOT_FOUND : MIXIT_E_IO;
        return {code, "cannot open '" + path_ + "': " + av_error_text(err)};
    }
    fmt_.reset(raw);
    if (const int e = avformat_find_stream_info(fmt_.get(), nullptr); e < 0) {
        return {MIXIT_E_NOT_SUPPORTED, "cannot read stream info of '" + path_ + "': " + av_error_text(e)};
    }
    start_ns_ = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time * 1000 : 0;  // AV_TIME_BASE = microseconds

    auto open_decoder = [&](AVMediaType type, int& index, AvPtr<AVCodecContext>& ctx) -> Status {
        const AVCodec* codec = nullptr;
        index = av_find_best_stream(fmt_.get(), type, -1, -1, &codec, 0);
        if (index < 0 || codec == nullptr) {
            index = -1;
            return Status::ok();  // stream type absent
        }
        ctx.reset(avcodec_alloc_context3(codec));
        if (!ctx) return {MIXIT_E_OUT_OF_MEMORY, "avcodec_alloc_context3 failed"};
        avcodec_parameters_to_context(ctx.get(), fmt_->streams[index]->codecpar);
        ctx->thread_count = 0;  // automatic frame/slice threading
        if (const int e = avcodec_open2(ctx.get(), codec, nullptr); e < 0) {
            return {MIXIT_E_NOT_SUPPORTED, std::string("cannot open decoder ") + codec->name + ": " + av_error_text(e)};
        }
        return Status::ok();
    };
    if (Status s = open_decoder(AVMEDIA_TYPE_VIDEO, vstream_, vdec_); !s.is_ok()) return s;
    if (Status s = open_decoder(AVMEDIA_TYPE_AUDIO, astream_, adec_); !s.is_ok()) return s;
    if (vstream_ < 0 && astream_ < 0) return {MIXIT_E_NOT_SUPPORTED, "'" + path_ + "' has no decodable audio or video"};

    frame_.reset(av_frame_alloc());
    packet_.reset(av_packet_alloc());
    if (!frame_ || !packet_) return {MIXIT_E_OUT_OF_MEMORY, "cannot allocate FFmpeg frame/packet"};

    format_.has_video = vstream_ >= 0;
    format_.has_audio = astream_ >= 0;
    std::string desc = "file '" + path_ + "'";
    if (format_.has_video) {
        if (vdec_->width <= 0 || vdec_->height <= 0 || vdec_->width > 8192 || vdec_->height > 8192) {
            return {MIXIT_E_NOT_SUPPORTED, "unsupported video size in '" + path_ + "'"};
        }
        format_.width = static_cast<std::uint32_t>(vdec_->width);
        format_.height = static_cast<std::uint32_t>(vdec_->height);
        const AVRational fr = av_guess_frame_rate(fmt_.get(), fmt_->streams[vstream_], nullptr);
        if (fr.num > 0 && fr.den > 0) {
            format_.nominal_frame_rate = {static_cast<std::uint32_t>(fr.num), static_cast<std::uint32_t>(fr.den)};
            nominal_frame_ns_ = duration_of_index(0, format_.nominal_frame_rate);
        }
        desc += " video " + std::to_string(format_.width) + "x" + std::to_string(format_.height) + " " +
                avcodec_get_name(vdec_->codec_id);
    }
    if (format_.has_audio) {
        desc += " audio " + std::to_string(adec_->sample_rate) + " Hz x" + std::to_string(adec_->ch_layout.nb_channels) +
                " " + avcodec_get_name(adec_->codec_id);
    }
    format_.description = desc + (options_.loop ? " (loop)" : "");
    return Status::ok();
}

bool FileSource::restart(SourceIo& io) noexcept {
    const std::int64_t target = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time : 0;
    if (avformat_seek_file(fmt_.get(), -1, std::numeric_limits<std::int64_t>::min(), target, target, 0) < 0 &&
        av_seek_frame(fmt_.get(), -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
        logger_.logf(MIXIT_LOG_ERROR, "file", "cannot seek to the start of '{}'", path_);
        return false;
    }
    if (vdec_) avcodec_flush_buffers(vdec_.get());
    if (adec_) {
        avcodec_flush_buffers(adec_.get());
        // Rebuild the resampler: drops its delay line from the previous pass.
        AVChannelLayout out_layout{};
        av_channel_layout_default(&out_layout, static_cast<int>(io.channels()));
        AVChannelLayout in_layout{};
        if (adec_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || adec_->ch_layout.nb_channels == 0) {
            av_channel_layout_default(&in_layout, std::max(1, adec_->ch_layout.nb_channels));
        } else {
            av_channel_layout_copy(&in_layout, &adec_->ch_layout);
        }
        SwrContext* swr = nullptr;
        const int e = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(io.sample_rate()),
                                          &in_layout, adec_->sample_fmt, adec_->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        swr_.reset(swr);
        if (e < 0 || swr_init(swr_.get()) < 0) {
            logger_.logf(MIXIT_LOG_ERROR, "file", "cannot configure audio resampler for '{}'", path_);
            swr_.reset();
        }
    }
    eof_ = false;
    loop_video_end_ns_ = 0;
    return true;
}

void FileSource::finish_block(SourceIo& io) noexcept {
    AudioBlock& b = *block_;
    b.frames = io.audio_block_frames();
    b.source_id = 0;
    b.pts_ns = io.origin_ns() + time_of_index(b.first_sample, Rate{io.sample_rate(), 1});
    audio_ring_.push(std::move(block_));
    block_.reset();
    block_filled_ = 0;
}

// Appends silence up to (not including) absolute sample `sample_index`.
void FileSource::emit_silence_until(SourceIo& io, std::int64_t sample_index) noexcept {
    const std::uint32_t ch = io.channels();
    while (next_sample_ < sample_index) {
        if (!block_) {
            while (audio_ring_.full()) {
                if (!push_earliest(io)) return;  // stopping
            }
            block_ = io.acquire_audio();
            if (!block_) return;
            block_->first_sample = next_sample_;
            block_filled_ = 0;
        }
        const auto room = static_cast<std::int64_t>(io.audio_block_frames() - block_filled_);
        const auto n = static_cast<std::uint32_t>(std::min(room, sample_index - next_sample_));
        std::fill_n(block_->samples + static_cast<std::size_t>(block_filled_) * ch, static_cast<std::size_t>(n) * ch, 0.0f);
        block_filled_ += n;
        next_sample_ += n;
        if (block_filled_ == io.audio_block_frames()) finish_block(io);
    }
}

void FileSource::convert_audio(SourceIo& io, const std::uint8_t** in, int in_count) noexcept {
    if (!swr_) return;
    const std::uint32_t ch = io.channels();
    bool first_call = true;
    for (;;) {
        if (!block_) {
            while (audio_ring_.full()) {
                if (!push_earliest(io)) return;  // stopping
            }
            block_ = io.acquire_audio();
            if (!block_) return;
            block_->first_sample = next_sample_;
            block_filled_ = 0;
        }
        const int room = static_cast<int>(io.audio_block_frames() - block_filled_);
        std::uint8_t* out = reinterpret_cast<std::uint8_t*>(block_->samples + static_cast<std::size_t>(block_filled_) * ch);
        const int produced = swr_convert(swr_.get(), &out, room, first_call ? in : nullptr, first_call ? in_count : 0);
        first_call = false;
        if (produced < 0) {
            io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        block_filled_ += static_cast<std::uint32_t>(produced);
        next_sample_ += produced;
        if (block_filled_ == io.audio_block_frames()) {
            finish_block(io);
            continue;  // more may be buffered inside the resampler
        }
        return;  // resampler has nothing more for now
    }
}

void FileSource::handle_audio_frame(SourceIo& io) noexcept {
    const AVFrame* f = frame_.get();
    const std::int64_t t = media_ns(f->best_effort_timestamp, fmt_->streams[astream_]->time_base);
    if (t != std::numeric_limits<std::int64_t>::min()) {
        // Fill gaps (late-starting audio, discontinuities) with silence so the
        // audio timeline stays locked to media time. Small jitter is ignored.
        const std::int64_t expected =
            loop_start_sample_ + muldiv_i64(std::max<std::int64_t>(0, t), io.sample_rate(), kNanosPerSecond, Rounding::Nearest).value_or(0);
        const std::int64_t buffered = swr_ ? swr_get_delay(swr_.get(), io.sample_rate()) : 0;
        if (expected > next_sample_ + buffered + static_cast<std::int64_t>(io.audio_block_frames())) {
            emit_silence_until(io, expected - buffered);
        }
    }
    convert_audio(io, const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
}

void FileSource::handle_video_frame(SourceIo& io) noexcept {
    const AVFrame* f = frame_.get();
    std::int64_t t = media_ns(f->best_effort_timestamp, fmt_->streams[vstream_]->time_base);
    if (t == std::numeric_limits<std::int64_t>::min()) t = last_video_media_ns_ + nominal_frame_ns_;
    last_video_media_ns_ = t;
    std::int64_t duration = nominal_frame_ns_;
    if (f->duration > 0) duration = media_ns(f->duration, fmt_->streams[vstream_]->time_base) + start_ns_;
    loop_video_end_ns_ = std::max(loop_video_end_ns_, t + duration);

    while (video_ring_.full()) {
        if (!push_earliest(io)) return;  // stopping
    }
    VideoRef out = io.acquire_video();
    if (!out) return;  // counted by SourceIo
    VideoFrame& v = *out;

    // Colour conversion to BGRA at the source size; BT.709 for HD material
    // when tagged, BT.601 otherwise; limited/full range from the stream.
    SwsContext* sws = sws_getCachedContext(sws_.release(), f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                           static_cast<int>(v.width), static_cast<int>(v.height), AV_PIX_FMT_BGRA,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    sws_.reset(sws);
    if (sws == nullptr) {
        io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    const int src_full = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    sws_setColorspaceDetails(sws, sws_getCoefficients(cs), src_full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);
    std::uint8_t* dst[4] = {v.data, nullptr, nullptr, nullptr};
    const int dst_stride[4] = {static_cast<int>(v.stride), 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dst_stride);

    v.pts_ns = io.origin_ns() + loop_offset_ns_ + t;
    v.duration_ns = duration;
    v.sequence = frame_counter_++;
    v.source_id = 0;
    v.flags = 0;
    video_ring_.push(std::move(out));
}

void FileSource::drain_decoder(SourceIo& io, AVCodecContext* dec, bool video) noexcept {
    while (avcodec_receive_frame(dec, frame_.get()) == 0) {
        if (video) handle_video_frame(io);
        else handle_audio_frame(io);
        av_frame_unref(frame_.get());
    }
}

bool FileSource::decode_more(SourceIo& io) noexcept {
    const int r = av_read_frame(fmt_.get(), packet_.get());
    if (r < 0) {
        // End of file (or read error): flush both decoders and the resampler.
        if (r != AVERROR_EOF) logger_.logf(MIXIT_LOG_WARNING, "file", "read error in '{}': {}", path_, av_error_text(r));
        if (vdec_) {
            avcodec_send_packet(vdec_.get(), nullptr);
            drain_decoder(io, vdec_.get(), true);
        }
        if (adec_) {
            avcodec_send_packet(adec_.get(), nullptr);
            drain_decoder(io, adec_.get(), false);
            convert_audio(io, nullptr, 0);
        }
        return false;
    }
    AVCodecContext* dec = packet_->stream_index == vstream_ ? vdec_.get()
                          : packet_->stream_index == astream_ ? adec_.get()
                                                              : nullptr;
    if (dec != nullptr) {
        if (avcodec_send_packet(dec, packet_.get()) < 0) {
            io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
        }
        drain_decoder(io, dec, dec == vdec_.get());
    }
    av_packet_unref(packet_.get());
    return true;
}

bool FileSource::push_earliest(SourceIo& io) noexcept {
    const Rate sr{io.sample_rate(), 1};
    if (video_ring_.empty() && audio_ring_.empty()) return false;
    std::int64_t video_due = std::numeric_limits<std::int64_t>::max();
    std::int64_t audio_due = std::numeric_limits<std::int64_t>::max();
    if (!video_ring_.empty()) video_due = video_ring_.front()->pts_ns;
    if (!audio_ring_.empty()) {
        const AudioBlock& b = *audio_ring_.front();
        audio_due = io.origin_ns() + time_of_index(b.first_sample + b.frames, sr);
    }
    // Video goes out when its pts arrives, audio when its block has fully
    // "played in" (block end) — the way capture hardware delivers.
    if (!io.sleep_until(std::min(video_due, audio_due))) return false;
    if (video_due <= audio_due) io.push_video(video_ring_.pop());
    else io.push_audio(audio_ring_.pop());
    return true;
}

void FileSource::run(SourceIo& io) noexcept {
    video_ring_.clear();
    audio_ring_.clear();
    block_.reset();
    block_filled_ = 0;
    next_sample_ = 0;
    loop_start_sample_ = 0;
    loop_offset_ns_ = 0;
    frame_counter_ = 0;
    last_video_media_ns_ = 0;
    if (!restart(io)) return;
    const Rate sr{io.sample_rate(), 1};
    bool finished = false;

    while (!io.stop_requested()) {
        // Decode ahead within fixed bounds (the rings hold pooled buffers).
        while (!eof_ && !finished && video_ring_.size() < kVideoAhead - 1 && audio_ring_.size() < kAudioAhead - 8 &&
               ((format_.has_video && video_ring_.size() < 2) || (format_.has_audio && audio_ring_.size() < 4))) {
            if (!decode_more(io)) eof_ = true;
        }

        if (eof_ && !finished && video_ring_.empty() && audio_ring_.empty()) {
            // End of this pass: pad audio to the loop length, then loop or stop.
            const std::int64_t audio_end_ns = time_of_index(next_sample_ - loop_start_sample_, sr);
            const std::int64_t loop_ns = std::max(loop_video_end_ns_, format_.has_audio ? audio_end_ns : 0);
            const std::int64_t loop_end_sample =
                loop_start_sample_ + muldiv_i64(loop_ns, io.sample_rate(), kNanosPerSecond, Rounding::Nearest).value_or(0);
            if (format_.has_audio) {
                emit_silence_until(io, loop_end_sample);
                if (next_sample_ < loop_end_sample) continue;  // stopping or pool exhausted
            }
            if (options_.loop && loop_ns > 0) {
                // The next pass starts exactly at the loop's end sample (a
                // partially filled block simply continues). With audio, the
                // video offset is derived from that sample index, so rounding
                // never accumulates across loops.
                loop_start_sample_ = format_.has_audio ? loop_end_sample : next_sample_;
                loop_offset_ns_ = format_.has_audio ? time_of_index(loop_end_sample, sr) : loop_offset_ns_ + loop_ns;
                io.counters().loops.fetch_add(1, std::memory_order_relaxed);
                if (!restart(io)) finished = true;
            } else {
                // Not looping: complete the last partial block with silence and
                // hold the last frame (frame sync keeps showing it).
                if (block_ && block_filled_ > 0) {
                    emit_silence_until(io, next_sample_ + (io.audio_block_frames() - block_filled_));
                }
                finished = true;
            }
            continue;
        }

        if (video_ring_.empty() && audio_ring_.empty()) {
            if (finished && !io.sleep_until(std::numeric_limits<std::int64_t>::max())) break;
            continue;  // nothing decoded yet (e.g. a packet without output); decode more
        }
        if (!push_earliest(io)) break;
    }
    video_ring_.clear();
    audio_ring_.clear();
    block_.reset();
}

Status open_file_source(const std::string& path_utf8, const FileSourceOptions& options, Logger& logger,
                        std::unique_ptr<MediaSource>& out) {
    if (path_utf8.empty()) return {MIXIT_E_INVALID_ARGUMENT, "path is empty"};
    auto source = std::make_unique<FileSource>(path_utf8, options, logger);
    if (Status s = source->open(); !s.is_ok()) return s;
    out = std::move(source);
    return Status::ok();
}

}  // namespace mixit::media

#else  // !MIXIT_HAVE_FFMPEG

namespace mixit::media {

Status open_file_source(const std::string&, const FileSourceOptions&, Logger&, std::unique_ptr<MediaSource>&) {
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without FFmpeg: file inputs are unavailable"};
}

}  // namespace mixit::media

#endif
