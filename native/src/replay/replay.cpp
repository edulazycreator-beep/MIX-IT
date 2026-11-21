#include "replay/replay.hpp"

#include <algorithm>
#include <cstring>

#include "logging/logger.hpp"
#include "media/ffmpeg_library.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

#if MIXIT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

namespace mixit::replay {

namespace {
constexpr std::size_t kQueueDepth = 3;
}

#if MIXIT_HAVE_FFMPEG

// Full-range (JPEG) YUV on either side of a conversion; RGB is always full range.
static void set_full_range(SwsContext* sws, bool src_full, bool dst_full) {
    int* inv = nullptr;
    int* table = nullptr;
    int src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
    if (sws_getColorspaceDetails(sws, &inv, &src_range, &table, &dst_range, &brightness, &contrast, &saturation) < 0) return;
    const int* bt601 = sws_getCoefficients(SWS_CS_ITU601);
    sws_setColorspaceDetails(sws, bt601, src_full ? 1 : 0, bt601, dst_full ? 1 : 0, brightness, contrast, saturation);
}

// ---------------------------------------------------------------------------- encoder

struct ReplayBuffer::Encoder {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    std::uint32_t width = 0, height = 0;

    ~Encoder() { close(); }
    void close() noexcept {
        avcodec_free_context(&ctx);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        sws_freeContext(sws);
        sws = nullptr;
    }
    Status open(std::uint32_t w, std::uint32_t h, std::uint32_t quality) {
        close();
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (codec == nullptr) return {MIXIT_E_NOT_SUPPORTED, "no MJPEG encoder in this FFmpeg build"};
        ctx = avcodec_alloc_context3(codec);
        ctx->width = static_cast<int>(w & ~1u);
        ctx->height = static_cast<int>(h & ~1u);
        ctx->time_base = AVRational{1, 1000};
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = static_cast<int>(quality) * FF_QP2LAMBDA;
        ctx->color_range = AVCOL_RANGE_JPEG;
        if (int err = avcodec_open2(ctx, codec, nullptr); err < 0) return {MIXIT_E_NOT_SUPPORTED, "cannot open the MJPEG encoder"};
        frame = av_frame_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = ctx->width;
        frame->height = ctx->height;
        if (av_frame_get_buffer(frame, 0) < 0) return {MIXIT_E_OUT_OF_MEMORY, "replay frame"};
        pkt = av_packet_alloc();
        width = w;
        height = h;
        return Status::ok();
    }
};

Status ReplayBuffer::create(std::uint32_t seconds, std::uint32_t quality, Logger& logger, std::shared_ptr<ReplayBuffer>& out) {
    if (!media::FfmpegLibrary::info().usable()) return {MIXIT_E_NOT_SUPPORTED, "replay needs FFmpeg (not available)"};
    if (seconds < 5 || seconds > 600) return {MIXIT_E_INVALID_ARGUMENT, "replay length must be 5..600 seconds"};
    if (quality < 2 || quality > 31) return {MIXIT_E_INVALID_ARGUMENT, "replay quality must be 2 (best)..31"};
    std::shared_ptr<ReplayBuffer> b(new ReplayBuffer(seconds, quality, logger));
    b->thread_ = std::thread([raw = b.get()] { raw->run(); });
    out = std::move(b);
    return Status::ok();
}

ReplayBuffer::ReplayBuffer(std::uint32_t seconds, std::uint32_t quality, Logger& logger)
    : span_ns_(static_cast<std::int64_t>(seconds) * 1'000'000'000), quality_(quality), logger_(logger),
      encoder_(std::make_unique<Encoder>()) {}

ReplayBuffer::~ReplayBuffer() {
    {
        std::lock_guard lock(queue_mutex_);
        stop_ = true;
    }
    queue_cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void ReplayBuffer::feed(const media::VideoRef& frame, std::int64_t pts_ns) noexcept {
    if (!frame) return;
    {
        std::unique_lock lock(queue_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || detached_ || queue_.size() >= kQueueDepth) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.emplace_back(frame, pts_ns);
    }
    queue_cv_.notify_one();
}

void ReplayBuffer::run() noexcept {
    os::set_current_thread_name("mixit-replay");
    for (;;) {
        std::pair<media::VideoRef, std::int64_t> item;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            in_flight_ = true;
        }
        // Releases the pool frame (before detach() can return).
        auto release = [&] {
            item.first.reset();
            {
                std::lock_guard lock(queue_mutex_);
                in_flight_ = false;
            }
            released_cv_.notify_all();
        };
        const media::VideoFrame& f = *item.first;
        const std::uint32_t fw = f.width, fh = f.height;
        Encoder& e = *encoder_;
        if (e.ctx == nullptr || e.width != f.width || e.height != f.height) {
            if (Status st = e.open(f.width, f.height, quality_); !st.is_ok()) {
                logger_.logf(MIXIT_LOG_ERROR, "replay", "replay encoder: {}", st.message());
                dropped_.fetch_add(1, std::memory_order_relaxed);
                release();
                continue;
            }
        }
        // YUVJ420P has the YUV420P layout; ask swscale for full range explicitly
        // (it deprecates the J formats).
        e.sws = sws_getCachedContext(e.sws, static_cast<int>(f.width), static_cast<int>(f.height), AV_PIX_FMT_BGRA, e.ctx->width,
                                     e.ctx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (e.sws == nullptr || av_frame_make_writable(e.frame) < 0) {
            release();
            continue;
        }
        set_full_range(e.sws, /*src_full=*/true, /*dst_full=*/true);
        const std::uint8_t* src[1] = {f.data};
        const int stride[1] = {static_cast<int>(f.stride)};
        sws_scale(e.sws, src, stride, 0, static_cast<int>(f.height), e.frame->data, e.frame->linesize);
        release();  // give the frame back to the session early; f is invalid from here
        e.frame->pts = item.second / 1'000'000;
        if (avcodec_send_frame(e.ctx, e.frame) < 0) continue;
        while (avcodec_receive_packet(e.ctx, e.pkt) >= 0) {
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(e.pkt->data, e.pkt->data + e.pkt->size);
            av_packet_unref(e.pkt);
            std::lock_guard lock(frames_mutex_);
            bytes_ += bytes->size();
            frames_.push_back(ReplayFrame{item.second, std::move(bytes)});
            width_ = fw;
            height_ = fh;
            while (!frames_.empty() && frames_.back().pts_ns - frames_.front().pts_ns > span_ns_) {
                bytes_ -= frames_.front().jpeg->size();
                frames_.pop_front();
            }
        }
    }
}

void ReplayBuffer::detach() noexcept {
    std::unique_lock lock(queue_mutex_);
    detached_ = true;
    queue_.clear();
    released_cv_.wait(lock, [&] { return !in_flight_; });
}

BufferInfo ReplayBuffer::info() const {
    std::lock_guard lock(frames_mutex_);
    BufferInfo i;
    if (!frames_.empty()) {
        i.oldest_ns = frames_.front().pts_ns;
        i.newest_ns = frames_.back().pts_ns;
    }
    i.frames = frames_.size();
    i.bytes = bytes_;
    i.dropped = dropped_.load();
    i.width = width_;
    i.height = height_;
    return i;
}

bool ReplayBuffer::frame_at(std::int64_t at_ns, ReplayFrame& out) const {
    std::lock_guard lock(frames_mutex_);
    if (frames_.empty() || frames_.front().pts_ns > at_ns) {
        if (frames_.empty()) return false;
        out = frames_.front();  // before the buffer start: the oldest frame
        return true;
    }
    const auto it = std::upper_bound(frames_.begin(), frames_.end(), at_ns,
                                     [](std::int64_t t, const ReplayFrame& f) { return t < f.pts_ns; });
    out = *(it - 1);
    return true;
}

// ---------------------------------------------------------------------------- playback

struct ReplaySource::Decoder {
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    ~Decoder() {
        avcodec_free_context(&ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        sws_freeContext(sws);
    }
    bool open() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (codec == nullptr) return false;
        ctx = avcodec_alloc_context3(codec);
        if (avcodec_open2(ctx, codec, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return true;
    }
    bool decode(const std::vector<std::uint8_t>& jpeg, media::VideoFrame& dst) {
        if (av_new_packet(pkt, static_cast<int>(jpeg.size())) < 0) return false;
        std::memcpy(pkt->data, jpeg.data(), jpeg.size());
        const int sent = avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        if (sent < 0 || avcodec_receive_frame(ctx, frame) < 0) return false;
        auto fmt = static_cast<AVPixelFormat>(frame->format);
        const bool full = fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P ||
                          frame->color_range == AVCOL_RANGE_JPEG;
        if (fmt == AV_PIX_FMT_YUVJ420P) fmt = AV_PIX_FMT_YUV420P;
        else if (fmt == AV_PIX_FMT_YUVJ422P) fmt = AV_PIX_FMT_YUV422P;
        else if (fmt == AV_PIX_FMT_YUVJ444P) fmt = AV_PIX_FMT_YUV444P;
        sws = sws_getCachedContext(sws, frame->width, frame->height, fmt, static_cast<int>(dst.width),
                                   static_cast<int>(dst.height), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) return false;
        set_full_range(sws, full, true);
        std::uint8_t* out[1] = {dst.data};
        const int stride[1] = {static_cast<int>(dst.stride)};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, out, stride);
        return true;
    }
};

#else  // no FFmpeg

struct ReplayBuffer::Encoder {};
struct ReplaySource::Decoder {
    bool open() { return false; }
    bool decode(const std::vector<std::uint8_t>&, media::VideoFrame&) { return false; }
};
Status ReplayBuffer::create(std::uint32_t, std::uint32_t, Logger&, std::shared_ptr<ReplayBuffer>&) {
    return {MIXIT_E_NOT_SUPPORTED, "replay needs FFmpeg"};
}
ReplayBuffer::ReplayBuffer(std::uint32_t seconds, std::uint32_t quality, Logger& logger)
    : span_ns_(static_cast<std::int64_t>(seconds) * 1'000'000'000), quality_(quality), logger_(logger) {}
ReplayBuffer::~ReplayBuffer() = default;
void ReplayBuffer::feed(const media::VideoRef&, std::int64_t) noexcept {}
void ReplayBuffer::detach() noexcept {}
void ReplayBuffer::run() noexcept {}
BufferInfo ReplayBuffer::info() const { return {}; }
bool ReplayBuffer::frame_at(std::int64_t, ReplayFrame&) const { return false; }

#endif

ReplaySource::ReplaySource(std::shared_ptr<ReplayBuffer> buffer, std::uint32_t width, std::uint32_t height,
                           std::uint32_t source_input)
    : buffer_(std::move(buffer)), source_input_(source_input) {
    format_.has_video = true;
    format_.has_audio = false;
    format_.width = width;
    format_.height = height;
    format_.nominal_frame_rate = media::Rate{30, 1};
    format_.description = "replay of input " + std::to_string(source_input);
}

ReplaySource::~ReplaySource() = default;

std::int64_t ReplaySource::position_locked(std::int64_t now) const noexcept {
    if (!active_ || paused_) return anchor_pos_ns_;
    return anchor_pos_ns_ + static_cast<std::int64_t>(static_cast<double>(now - anchor_now_ns_) * static_cast<double>(speed_));
}

Status ReplaySource::play(std::int64_t from_ns, float speed, bool paused) {
    if (!(speed >= 0.1f && speed <= 2.0f)) return {MIXIT_E_INVALID_ARGUMENT, "replay speed must be 0.1..2"};
    std::lock_guard lock(mutex_);
    active_ = true;
    paused_ = paused;
    speed_ = speed;
    anchor_pos_ns_ = from_ns;
    anchor_now_ns_ = static_cast<std::int64_t>(monotonic_ns());
    generation_.fetch_add(1);
    return Status::ok();
}

Status ReplaySource::set_speed(float speed) {
    if (!(speed >= 0.1f && speed <= 2.0f)) return {MIXIT_E_INVALID_ARGUMENT, "replay speed must be 0.1..2"};
    const auto now = static_cast<std::int64_t>(monotonic_ns());
    std::lock_guard lock(mutex_);
    anchor_pos_ns_ = position_locked(now);
    anchor_now_ns_ = now;
    speed_ = speed;
    return Status::ok();
}

Status ReplaySource::set_paused(bool paused) {
    const auto now = static_cast<std::int64_t>(monotonic_ns());
    std::lock_guard lock(mutex_);
    if (!active_) return {MIXIT_E_INVALID_STATE, "nothing is being replayed"};
    anchor_pos_ns_ = position_locked(now);
    anchor_now_ns_ = now;
    paused_ = paused;
    return Status::ok();
}

ReplaySource::State ReplaySource::state() const {
    std::lock_guard lock(mutex_);
    State s;
    s.active = active_;
    s.paused = paused_;
    s.speed = speed_;
    s.position_ns = position_locked(static_cast<std::int64_t>(monotonic_ns()));
    return s;
}

void ReplaySource::run(media::SourceIo& io) noexcept {
    os::set_current_thread_name("mixit-replay-out");
    Decoder decoder;
    if (!decoder.open()) return;
    std::int64_t shown_pts = -1;
    std::uint64_t shown_generation = ~0ull;
    while (!io.stop_requested()) {
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        std::int64_t position = 0;
        bool active = false;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            active = active_;
            position = position_locked(now);
            generation = generation_.load();
        }
        ReplayFrame f;
        if (active && buffer_->frame_at(position, f) && f.jpeg && (f.pts_ns != shown_pts || generation != shown_generation)) {
            if (media::VideoRef out = io.acquire_video()) {
                if (decoder.decode(*f.jpeg, *out)) {
                    out->pts_ns = now;
                    out->duration_ns = 0;
                    out->flags = 0;
                    out->sequence = f.pts_ns;
                    io.push_video(std::move(out));
                    shown_pts = f.pts_ns;
                    shown_generation = generation;
                }
            }
        }
        if (!io.sleep_until(static_cast<std::int64_t>(monotonic_ns()) + 5'000'000)) break;
    }
}

}  // namespace mixit::replay
