#include "output/output.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <thread>

#include "logging/logger.hpp"
#include "media/ffmpeg_library.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

#if MIXIT_HAVE_FFMPEG

#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <semaphore>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace mixit::output {

namespace fs = std::filesystem;

namespace {

constexpr AVRational kNs{1, 1'000'000'000};
constexpr std::int64_t kNetworkQueueNs = 4'000'000'000;   // a destination may fall this far behind
constexpr std::int64_t kFileQueueNs = 20'000'000'000;
constexpr std::int64_t kMinFreeBytes = 256ll * 1024 * 1024;
constexpr std::uint32_t kVideoQueue = 2;                   // frames waiting for the encoder
constexpr std::uint32_t kAudioChunks = 256;                // 2.56 s of 10 ms blocks

std::string av_error_text(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

fs::path utf8_path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

bool is_network_url(const std::string& url) {
    const std::string u = lower(url);
    return starts_with(u, "rtmp://") || starts_with(u, "rtmps://") || starts_with(u, "srt://") ||
           starts_with(u, "udp://") || starts_with(u, "rtp://") || starts_with(u, "tcp://") ||
           starts_with(u, "http://") || starts_with(u, "https://");
}

// Container for a url (or the explicit format).
Status resolve_format(const OutputConfig& c, std::string& out) {
    static constexpr const char* kKnown[] = {"mp4", "mov", "matroska", "mpegts", "flv", "hls"};
    if (!c.format.empty()) {
        if (std::find_if(std::begin(kKnown), std::end(kKnown), [&](const char* k) { return c.format == k; }) ==
            std::end(kKnown)) {
            return {MIXIT_E_INVALID_ARGUMENT, "unknown output format '" + c.format + "'"};
        }
        out = c.format;
        return Status::ok();
    }
    const std::string u = lower(c.url);
    auto ends = [&](const char* ext) { return u.size() >= std::strlen(ext) && u.compare(u.size() - std::strlen(ext), std::string::npos, ext) == 0; };
    if (starts_with(u, "rtmp://") || starts_with(u, "rtmps://")) out = "flv";
    else if (starts_with(u, "srt://") || starts_with(u, "udp://") || starts_with(u, "rtp://") || starts_with(u, "tcp://")) out = "mpegts";
    else if (ends(".m3u8")) out = "hls";
    else if (ends(".mp4")) out = "mp4";
    else if (ends(".mov")) out = "mov";
    else if (ends(".mkv")) out = "matroska";
    else if (ends(".ts")) out = "mpegts";
    else if (ends(".flv")) out = "flv";
    else return {MIXIT_E_INVALID_ARGUMENT, "cannot tell the format from the url; set it explicitly"};
    return Status::ok();
}

// FFmpeg's own messages (stderr) can contain urls, e.g. in protocol
// errors. Replace every url in them with its redacted form (stream keys,
// passwords, query values) before printing.
void redacting_log_callback(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > av_log_get_level()) return;
    char line[1024];
    int print_prefix = 1;
    va_list copy;
    va_copy(copy, vl);
    av_log_format_line2(avcl, level, fmt, copy, line, sizeof(line), &print_prefix);
    va_end(copy);
    std::string text = line;
    std::size_t pos = 0;
    while ((pos = text.find("://", pos)) != std::string::npos) {
        std::size_t begin = pos;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) != 0 || text[begin - 1] == '+')) --begin;
        std::size_t end = pos + 3;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\'' && text[end] != '"') ++end;
        const std::string redacted = redact_url(text.substr(begin, end - begin));
        text.replace(begin, end - begin, redacted);
        pos = begin + redacted.size();
    }
    std::fputs(text.c_str(), stderr);
}

void install_log_redaction() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(&redacting_log_callback); });
}

// H.264 encoders in order of preference: hardware first, then software.
constexpr const char* kCandidates[] = {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libopenh264", "libx264"};

AVPixelFormat pick_pixel_format(const AVCodec* codec) {
    const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* list = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &count) >= 0) {
        formats = static_cast<const AVPixelFormat*>(list);
    }
#else
    formats = codec->pix_fmts;
#endif
    if (formats == nullptr) return AV_PIX_FMT_YUV420P;
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_YUV420P) return *f;
    }
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_NV12) return *f;
    }
    return formats[0];
}

struct CodecCtxDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;

// Opens one video encoder with MIX IT's live settings (CBR-like, no B-frames,
// fixed GOP, BT.709 limited range, global headers).
Status open_video_encoder(const char* name, std::uint32_t width, std::uint32_t height, media::Rate rate,
                          std::uint32_t kbps, std::uint32_t keyframe_ms, CodecCtxPtr& out) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr || codec->type != AVMEDIA_TYPE_VIDEO) {
        return {MIXIT_E_NOT_FOUND, std::string("encoder ") + name + " is not in this FFmpeg build"};
    }
    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) return {MIXIT_E_OUT_OF_MEMORY, "avcodec_alloc_context3"};
    ctx->width = static_cast<int>(width);
    ctx->height = static_cast<int>(height);
    ctx->time_base = AVRational{static_cast<int>(rate.den), static_cast<int>(rate.num)};
    ctx->framerate = AVRational{static_cast<int>(rate.num), static_cast<int>(rate.den)};
    ctx->pix_fmt = pick_pixel_format(codec);
    ctx->bit_rate = static_cast<std::int64_t>(kbps) * 1000;
    ctx->rc_max_rate = ctx->bit_rate;
    ctx->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(ctx->bit_rate, INT32_MAX));
    const double fps = rate.to_double();
    ctx->gop_size = std::max(1, static_cast<int>(fps * keyframe_ms / 1000.0 + 0.5));
    ctx->keyint_min = ctx->gop_size;
    ctx->max_b_frames = 0;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* opts = nullptr;
    const std::string n = name;
    if (n == "h264_nvenc") {
        av_dict_set(&opts, "preset", "p4", 0);
        av_dict_set(&opts, "tune", "ll", 0);
        av_dict_set(&opts, "rc", "cbr", 0);
        av_dict_set(&opts, "forced-idr", "1", 0);
    } else if (n == "h264_qsv") {
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "forced_idr", "1", 0);
    } else if (n == "h264_amf") {
        av_dict_set(&opts, "usage", "lowlatency", 0);
        av_dict_set(&opts, "quality", "speed", 0);
        av_dict_set(&opts, "rc", "cbr", 0);
    } else if (n == "h264_mf") {
        av_dict_set(&opts, "rate_control", "cbr", 0);
        av_dict_set(&opts, "scenario", "live_streaming", 0);
    } else if (n == "libopenh264") {
        av_dict_set(&opts, "rc_mode", "bitrate", 0);
        av_dict_set(&opts, "allow_skip_frames", "0", 0);
    } else if (n == "libx264") {
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
    }
    const int err = avcodec_open2(ctx.get(), codec, &opts);
    av_dict_free(&opts);
    if (err < 0) return {MIXIT_E_NOT_SUPPORTED, std::string(name) + ": " + av_error_text(err)};
    out = std::move(ctx);
    return Status::ok();
}

bool is_hardware(const char* name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    return codec != nullptr && (codec->capabilities & AV_CODEC_CAP_HARDWARE) != 0;
}

}  // namespace

Status query_video_encoders(std::vector<EncoderInfo>& out) {
    out.clear();
    if (!media::FfmpegLibrary::info().usable()) return {MIXIT_E_NOT_SUPPORTED, "FFmpeg is not available"};
    av_log_set_level(AV_LOG_QUIET);  // probing failing hardware encoders is noisy by nature
    for (const char* name : kCandidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (codec == nullptr) continue;
        EncoderInfo e;
        e.name = name;
        e.long_name = codec->long_name != nullptr ? codec->long_name : "";
        e.hardware = (codec->capabilities & AV_CODEC_CAP_HARDWARE) != 0;
        CodecCtxPtr ctx;
        const Status st = open_video_encoder(name, 1280, 720, media::Rate{30, 1}, 3000, 2000, ctx);
        e.usable = st.is_ok();
        if (!e.usable) e.error = st.message();
        out.push_back(std::move(e));
    }
    av_log_set_level(AV_LOG_WARNING);
    return Status::ok();
}

// ---------------------------------------------------------------------------------- Sink

struct StreamSetup {
    const AVCodecParameters* video = nullptr;
    const AVCodecParameters* audio = nullptr;  // null: no audio stream
    AVRational video_tb{1, 30};
    AVRational audio_tb{1, 48000};
};

// One destination: its own thread, queue, muxer and recovery policy.
class Sink {
public:
    Sink(std::uint32_t id, OutputConfig config, std::string format, Logger& logger, std::function<void()> want_keyframe)
        : id_(id), config_(std::move(config)), format_(std::move(format)), logger_(logger),
          network_(is_network_url(config_.url)), want_keyframe_(std::move(want_keyframe)) {}

    ~Sink() {
        abort_.store(true);
        request_stop();
        if (thread_.joinable()) thread_.join();
        clear_queue();
        avcodec_parameters_free(&video_par_);
        avcodec_parameters_free(&audio_par_);
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    void start(const StreamSetup& s, const std::string& encoder, bool hardware) {
        video_par_ = avcodec_parameters_alloc();
        avcodec_parameters_copy(video_par_, s.video);
        if (s.audio != nullptr) {
            audio_par_ = avcodec_parameters_alloc();
            avcodec_parameters_copy(audio_par_, s.audio);
        }
        video_tb_ = s.video_tb;
        audio_tb_ = s.audio_tb;
        {
            std::lock_guard lock(mutex_);
            stats_.state = OutputState::Starting;
            stats_.encoder = encoder;
            stats_.hardware_encoder = hardware;
        }
        thread_ = std::thread([this] { run(); });
    }

    // Encoder thread: takes ownership of `pkt`.
    void push(AVPacket* pkt, int stream) noexcept {
        const AVRational tb = stream == 0 ? video_tb_ : audio_tb_;
        const std::int64_t ns = av_rescale_q(pkt->pts, tb, kNs);
        const bool key = stream == 0 && (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        std::lock_guard lock(mutex_);
        if (stopping_) {  // nothing new after a stop request
            av_packet_free(&pkt);
            return;
        }
        if (waiting_key_ && !key) {
            ++stats_.dropped_packets;
            av_packet_free(&pkt);
            return;
        }
        waiting_key_ = false;
        if (!queue_.empty() && ns - queue_.front().ns > (network_ ? kNetworkQueueNs : kFileQueueNs)) {
            // This destination cannot keep up: drop what it has queued and
            // resume at the next keyframe. Other destinations are unaffected.
            stats_.dropped_packets += queue_.size();
            if (!network_) stats_.data_lost = true;
            for (Item& it : queue_) av_packet_free(&it.pkt);
            queue_.clear();
            drop_logged_ = true;
            if (!key) {
                waiting_key_ = true;
                ++stats_.dropped_packets;
                av_packet_free(&pkt);
                cv_.notify_one();
                return;
            }
        }
        queue_.push_back(Item{pkt, stream, ns});
        cv_.notify_one();
    }

    void request_stop() noexcept {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        stop_flag_.store(true);
        cv_.notify_one();
    }
    void join() noexcept {
        if (thread_.joinable()) thread_.join();
    }
    void abort() noexcept { abort_.store(true); }
    [[nodiscard]] bool finished() const noexcept { return finished_.load(); }

    OutputStats stats() const {
        std::lock_guard lock(mutex_);
        OutputStats s = stats_;
        if (!queue_.empty()) s.queue_ms = static_cast<std::uint32_t>((queue_.back().ns - queue_.front().ns) / 1'000'000);
        return s;
    }

private:
    struct Item {
        AVPacket* pkt;
        int stream;
        std::int64_t ns;
    };

    static int interrupt_cb(void* opaque) {
        auto* self = static_cast<Sink*>(opaque);
        if (self->abort_.load(std::memory_order_relaxed)) return 1;
        // A stop while still connecting cancels the attempt (nothing to drain).
        if (self->stop_flag_.load(std::memory_order_relaxed) && !self->connected_.load(std::memory_order_relaxed)) return 1;
        const std::int64_t deadline = self->op_deadline_ns_.load(std::memory_order_relaxed);
        return deadline != 0 && static_cast<std::int64_t>(monotonic_ns()) > deadline ? 1 : 0;
    }
    void arm(std::int64_t ms) noexcept {
        op_deadline_ns_.store(network_ ? static_cast<std::int64_t>(monotonic_ns()) + ms * 1'000'000 : 0);
    }

    void set_state(OutputState s, const std::string& error = {}) {
        std::lock_guard lock(mutex_);
        stats_.state = s;
        if (!error.empty()) stats_.last_error = error;
    }

    void clear_queue() noexcept {
        std::lock_guard lock(mutex_);
        for (Item& it : queue_) av_packet_free(&it.pkt);
        queue_.clear();
    }

    std::string target_name() const {
        if (network_ || config_.segment_seconds == 0) return config_.url;
        const fs::path p = utf8_path(config_.url);
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%04u", segment_index_);
        fs::path seg = p.parent_path() / (p.stem().u8string() + reinterpret_cast<const char8_t*>(suffix) + p.extension().u8string());
        const std::u8string u8 = seg.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

    // Recordings never overwrite an existing file: "name (2).ext", ...
    static std::string unused_file_name(const std::string& name) {
        std::error_code ec;
        const fs::path p = utf8_path(name);
        if (!fs::exists(p, ec)) return name;
        for (int n = 2; n < 10'000; ++n) {
            const std::u8string suffix = u8" (" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(n).c_str())) + u8")";
            const fs::path candidate = p.parent_path() / (p.stem().u8string() + suffix + p.extension().u8string());
            if (!fs::exists(candidate, ec)) {
                const std::u8string u8 = candidate.u8string();
                return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
            }
        }
        return name;
    }

    Status open_output() {
        std::string url = target_name();
        if (!network_ && format_ != "hls") {
            url = unused_file_name(url);
            // Create the folder if needed (a recording must not fail on a missing folder).
            std::error_code ec;
            const fs::path dir = utf8_path(url).parent_path();
            if (!dir.empty()) fs::create_directories(dir, ec);
        }
        AVFormatContext* oc = nullptr;
        int err = avformat_alloc_output_context2(&oc, nullptr, format_.c_str(), url.c_str());
        if (err < 0 || oc == nullptr) return {MIXIT_E_INTERNAL, "cannot create the " + format_ + " muxer: " + av_error_text(err)};
        oc_ = oc;
        oc_->interrupt_callback = AVIOInterruptCB{&Sink::interrupt_cb, this};
        AVStream* vs = avformat_new_stream(oc_, nullptr);
        avcodec_parameters_copy(vs->codecpar, video_par_);
        vs->time_base = video_tb_;
        out_tb_[0] = video_tb_;
        if (audio_par_ != nullptr) {
            AVStream* as = avformat_new_stream(oc_, nullptr);
            avcodec_parameters_copy(as->codecpar, audio_par_);
            as->time_base = audio_tb_;
        }
        // Formats that do not carry codec headers out of band get SPS/PPS
        // repeated before every keyframe (decodable from any join point).
        bsf_ = nullptr;
        if ((oc_->oformat->flags & AVFMT_GLOBALHEADER) == 0 || format_ == "hls" || format_ == "mpegts") {
            const AVBitStreamFilter* f = av_bsf_get_by_name("dump_extra");
            if (f != nullptr && av_bsf_alloc(f, &bsf_) == 0) {
                avcodec_parameters_copy(bsf_->par_in, video_par_);
                bsf_->time_base_in = video_tb_;
                av_opt_set(bsf_->priv_data, "freq", "keyframe", 0);
                if (av_bsf_init(bsf_) < 0) av_bsf_free(&bsf_);
            }
        }
        AVDictionary* opts = nullptr;
        if ((format_ == "mp4" || format_ == "mov") && config_.fragmented) {
            av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        }
        if (format_ == "hls") {
            const std::uint32_t seconds = std::max(1u, (config_.keyframe_ms + 999) / 1000);
            av_dict_set_int(&opts, "hls_time", seconds, 0);
            av_dict_set_int(&opts, "hls_list_size", 6, 0);
            av_dict_set(&opts, "hls_flags", "delete_segments+independent_segments", 0);
        }
        if (format_ == "flv") av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
        if ((oc_->oformat->flags & AVFMT_NOFILE) == 0) {
            AVDictionary* io_opts = nullptr;
            if (network_) av_dict_set(&io_opts, "rw_timeout", "5000000", 0);
            arm(10'000);
            err = avio_open2(&oc_->pb, url.c_str(), AVIO_FLAG_WRITE, &oc_->interrupt_callback, &io_opts);
            av_dict_free(&io_opts);
            if (err < 0) {
                av_dict_free(&opts);
                close_output(false);
                return {MIXIT_E_IO, "cannot open " + redact_url(url) + ": " + av_error_text(err)};
            }
        }
        arm(10'000);
        err = avformat_write_header(oc_, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            close_output(false);
            return {MIXIT_E_IO, "cannot start " + redact_url(url) + ": " + av_error_text(err)};
        }
        connected_.store(true);
        out_tb_[0] = oc_->streams[0]->time_base;
        if (audio_par_ != nullptr) out_tb_[1] = oc_->streams[1]->time_base;
        started_ = false;
        segment_start_ns_ = 0;
        {
            std::lock_guard lock(mutex_);
            stats_.current_file = network_ ? std::string() : url;
            stats_.segments = network_ ? 0 : segment_index_;
        }
        return Status::ok();
    }

    void close_output(bool write_trailer) noexcept {
        if (oc_ == nullptr) return;
        if (write_trailer) {
            arm(5'000);
            const int err = av_write_trailer(oc_);
            if (err < 0) logger_.logf(MIXIT_LOG_WARNING, "output", "output {}: trailer: {}", id_, av_error_text(err));
        }
        if ((oc_->oformat->flags & AVFMT_NOFILE) == 0) avio_closep(&oc_->pb);
        avformat_free_context(oc_);
        oc_ = nullptr;
        av_bsf_free(&bsf_);
        for (Item& a : pending_audio_) av_packet_free(&a.pkt);
        pending_audio_.clear();
        connected_.store(false);  // only now: the trailer must not be interrupted by a stop
    }

    // Writes one packet (owned). Returns false on a write error.
    bool write(Item& it) {
        AVPacket* pkt = it.pkt;
        const int stream = it.stream;
        if (!started_) {
            if (stream == 1) {
                // Audio can come out of the encoders before the first video
                // packet (video encoders with latency): keep it, it may
                // belong after the keyframe that starts the file.
                pending_audio_.push_back(it);
                if (pending_audio_.size() > 128) {
                    av_packet_free(&pending_audio_.front().pkt);
                    pending_audio_.pop_front();
                }
                return true;
            }
            if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {  // every file/connection starts at a keyframe
                av_packet_free(&pkt);
                return true;
            }
            started_ = true;
            offset_ns_ = it.ns;
            offset_[0] = pkt->pts;
            offset_[1] = av_rescale_q_rnd(offset_ns_, kNs, audio_tb_,
                                          static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX));
            segment_start_ns_ = it.ns;
            if (!write_one(it)) return false;
            while (!pending_audio_.empty()) {  // now on the file's timeline (earlier ones are dropped there)
                Item a = pending_audio_.front();
                pending_audio_.pop_front();
                if (!write_one(a)) return false;
            }
            return true;
        }
        return write_one(it);
    }

    bool write_one(Item& it) {
        AVPacket* pkt = it.pkt;
        const int stream = it.stream;
        if (stream == 1 && audio_par_ == nullptr) {
            av_packet_free(&pkt);
            return true;
        }
        pkt->pts -= offset_[stream];
        pkt->dts -= offset_[stream];
        // Audio from before the first keyframe is dropped. Video dts may be
        // negative with reordering encoders; the muxer handles that without
        // moving the presentation times (A/V sync is in the pts).
        if (pkt->pts < 0 || (stream == 1 && pkt->dts < 0)) {
            av_packet_free(&pkt);
            return true;
        }
        pkt->stream_index = stream;
        const int bytes = pkt->size;
        int err = 0;
        arm(5'000);
        if (stream == 0 && bsf_ != nullptr) {
            err = av_bsf_send_packet(bsf_, pkt);  // takes the data; pkt is left blank
            av_packet_free(&pkt);
            while (err >= 0) {
                AVPacket* out = av_packet_alloc();
                if (out == nullptr || av_bsf_receive_packet(bsf_, out) < 0) {
                    av_packet_free(&out);
                    break;
                }
                av_packet_rescale_ts(out, video_tb_, out_tb_[0]);
                out->stream_index = 0;
                err = av_interleaved_write_frame(oc_, out);
                av_packet_free(&out);
            }
        } else {
            av_packet_rescale_ts(pkt, stream == 0 ? video_tb_ : audio_tb_, out_tb_[stream]);
            err = av_interleaved_write_frame(oc_, pkt);
            av_packet_free(&pkt);
        }
        if (err < 0) {
            std::lock_guard lock(mutex_);
            stats_.last_error = "write failed: " + av_error_text(err);
            return false;
        }
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        std::lock_guard lock(mutex_);
        stats_.bytes_written += static_cast<std::uint64_t>(bytes);
        (stream == 0 ? stats_.video_packets : stats_.audio_packets) += 1;
        stats_.duration_ns = std::max(stats_.duration_ns, it.ns - offset_ns_);
        window_bytes_ += static_cast<std::uint64_t>(bytes);
        if (window_start_ns_ == 0) window_start_ns_ = now;
        if (now - window_start_ns_ >= 2'000'000'000) {
            stats_.bitrate_kbps = static_cast<std::uint32_t>(window_bytes_ * 8 * 1'000'000 / static_cast<std::uint64_t>(now - window_start_ns_));
            window_bytes_ = 0;
            window_start_ns_ = now;
        }
        return true;
    }

    bool check_storage() {
        if (network_) return true;
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        if (now - last_space_check_ns_ < 2'000'000'000) return true;
        last_space_check_ns_ = now;
        std::error_code ec;
        fs::path dir = utf8_path(target_name()).parent_path();
        if (dir.empty()) dir = fs::current_path(ec);
        const fs::space_info si = fs::space(dir, ec);
        if (ec) return true;
        const auto free_bytes = static_cast<std::int64_t>(si.available);
        {
            std::lock_guard lock(mutex_);
            stats_.free_bytes = free_bytes;
        }
        return free_bytes >= kMinFreeBytes;
    }

    // Pops the next packet; false when stopping and drained.
    bool next(Item& out) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return !queue_.empty() || stopping_; });
        out = Item{nullptr, 0, 0};
        if (queue_.empty()) return !stopping_;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

    void backoff(std::uint32_t attempt) {
        const std::int64_t ms = std::min<std::int64_t>(30'000, 1000ll << std::min<std::uint32_t>(attempt, 5));
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, until, [&] { return stopping_; });
    }

    void run() noexcept {
        os::set_current_thread_name("mixit-out");
        std::uint32_t attempt = 0;
        bool failed = false;
        while (!abort_.load()) {
            {
                std::lock_guard lock(mutex_);
                if (stopping_ && queue_.empty()) break;
            }
            const Status st = open_output();
            if (!st.is_ok()) {
                if (stopping_now()) break;  // cancelled by a stop: not a failure
                logger_.logf(MIXIT_LOG_WARNING, "output", "output {}: {}", id_, st.message());
                if (network_ && config_.reconnect && !stopping_now()) {
                    {
                        std::lock_guard lock(mutex_);
                        stats_.state = OutputState::Reconnecting;
                        stats_.last_error = st.message();
                        ++stats_.reconnects;
                    }
                    backoff(attempt++);
                    continue;
                }
                set_state(OutputState::Failed, st.message());
                failed = true;
                break;
            }
            attempt = 0;
            set_state(OutputState::Running);
            if (want_keyframe_) want_keyframe_();
            logger_.logf(MIXIT_LOG_INFO, "output", "output {}: writing {} ({})", id_, network_ ? redact_url(config_.url) : target_name(), format_);
            bool write_error = false;
            bool next_segment = false;
            Item it{};
            while (next(it)) {
                if (it.pkt == nullptr) continue;
                if (drop_logged_.exchange(false)) {
                    logger_.logf(network_ ? MIXIT_LOG_WARNING : MIXIT_LOG_ERROR, "output",
                                 "output {}: destination fell behind; dropped queued packets up to the next keyframe", id_);
                }
                if (!network_ && config_.segment_seconds > 0 && started_ && it.stream == 0 &&
                    (it.pkt->flags & AV_PKT_FLAG_KEY) != 0 &&
                    it.ns - segment_start_ns_ >= static_cast<std::int64_t>(config_.segment_seconds) * 1'000'000'000) {
                    {
                        std::lock_guard lock(mutex_);
                        queue_.push_front(it);  // first packet of the next file
                    }
                    next_segment = true;
                    break;
                }
                if (!write(it)) {
                    write_error = true;
                    break;
                }
                if (!check_storage()) {
                    close_output(true);
                    set_state(OutputState::Failed, "stopped: less than 256 MB free on the recording drive");
                    logger_.logf(MIXIT_LOG_ERROR, "output", "output {}: recording stopped, drive almost full", id_);
                    failed = true;
                    break;
                }
            }
            if (failed) break;
            if (next_segment) {
                close_output(true);
                ++segment_index_;
                continue;
            }
            if (write_error) {
                const std::string err = stats().last_error;
                logger_.logf(MIXIT_LOG_WARNING, "output", "output {}: {}", id_, err);
                close_output(!network_);
                if (network_ && config_.reconnect && !stopping_now()) {
                    {
                        std::lock_guard lock(mutex_);
                        stats_.state = OutputState::Reconnecting;
                        ++stats_.reconnects;
                        waiting_key_ = true;
                        for (Item& q : queue_) av_packet_free(&q.pkt);
                        queue_.clear();
                    }
                    backoff(attempt++);
                    continue;
                }
                set_state(OutputState::Failed);
                {
                    std::lock_guard lock(mutex_);
                    if (!network_) stats_.data_lost = true;
                }
                failed = true;
                break;
            }
            // Stop requested and the queue is drained: finalize.
            set_state(OutputState::Stopping);
            close_output(true);
            break;
        }
        close_output(!abort_.load());
        clear_queue();
        if (!failed) set_state(OutputState::Idle);
        finished_.store(true);
    }

    bool stopping_now() {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    const std::uint32_t id_;
    const OutputConfig config_;
    const std::string format_;
    Logger& logger_;
    const bool network_;
    std::function<void()> want_keyframe_;

    AVCodecParameters* video_par_ = nullptr;
    AVCodecParameters* audio_par_ = nullptr;
    AVRational video_tb_{1, 30}, audio_tb_{1, 48000};
    AVRational out_tb_[2]{{1, 30}, {1, 48000}};

    // Sink thread only.
    AVFormatContext* oc_ = nullptr;
    AVBSFContext* bsf_ = nullptr;
    bool started_ = false;
    std::deque<Item> pending_audio_;  // audio before the first keyframe of a file/connection
    std::int64_t offset_ns_ = 0;
    std::int64_t offset_[2]{0, 0};
    std::int64_t segment_start_ns_ = 0;
    std::uint32_t segment_index_ = 1;
    std::int64_t last_space_check_ns_ = 0;
    std::uint64_t window_bytes_ = 0;
    std::int64_t window_start_ns_ = 0;

    std::thread thread_;
    std::atomic<bool> abort_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> drop_logged_{false};
    std::atomic<std::int64_t> op_deadline_ns_{0};
    std::atomic<bool> stop_flag_{false};   // mirrors stopping_ for the interrupt callback
    std::atomic<bool> connected_{false};   // header written on the current file/connection

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool stopping_ = false;
    bool waiting_key_ = false;
    OutputStats stats_;
};

// ---------------------------------------------------------------------------------- encoder pipeline

struct PipelineKey {
    std::uint32_t source = 0;
    std::string encoder;
    std::uint32_t video_kbps = 0, keyframe_ms = 0, audio_kbps = 0;
    friend bool operator==(const PipelineKey&, const PipelineKey&) = default;
};

class EncoderPipeline {
public:
    EncoderPipeline(PipelineKey key, const Timeline& tl, Logger& logger) : key_(std::move(key)), tl_(tl), logger_(logger) {
        chunk_frames_ = std::max(1u, tl_.sample_rate / 100);
        chunk_storage_.assign(static_cast<std::size_t>(kAudioChunks) * chunk_frames_ * tl_.channels, 0.0f);
    }
    ~EncoderPipeline() {
        stop_.store(true);
        wake_.release();
        if (thread_.joinable()) thread_.join();
        for (auto& v : vring_) v.ref.reset();
        av_frame_free(&vframe_);
        av_frame_free(&aframe_);
        av_packet_free(&pkt_);
        sws_freeContext(sws_);
        avcodec_parameters_free(&vpar_);
        avcodec_parameters_free(&apar_);
    }
    EncoderPipeline(const EncoderPipeline&) = delete;
    EncoderPipeline& operator=(const EncoderPipeline&) = delete;

    [[nodiscard]] const PipelineKey& key() const noexcept { return key_; }

    // Control thread: chooses and opens the encoders.
    Status open(std::uint32_t width, std::uint32_t height, bool with_audio) {
        width_ = width & ~1u;
        height_ = height & ~1u;
        if (width_ < 16 || height_ < 16) return {MIXIT_E_INVALID_ARGUMENT, "source too small to encode"};
        std::vector<std::string> tried;
        if (!key_.encoder.empty()) {
            if (Status st = open_video_encoder(key_.encoder.c_str(), width_, height_, tl_.frame_rate, key_.video_kbps,
                                               key_.keyframe_ms, video_);
                !st.is_ok()) {
                return st;
            }
            encoder_name_ = key_.encoder;
        } else {
            av_log_set_level(AV_LOG_QUIET);  // failing hardware probes are expected and noisy
            for (const char* name : kCandidates) {
                if (avcodec_find_encoder_by_name(name) == nullptr) continue;
                const Status st = open_video_encoder(name, width_, height_, tl_.frame_rate, key_.video_kbps, key_.keyframe_ms, video_);
                if (st.is_ok()) {
                    encoder_name_ = name;
                    break;
                }
                tried.push_back(st.message());
            }
            av_log_set_level(AV_LOG_WARNING);
            if (!video_) {
                std::string all;
                for (const auto& t : tried) all += (all.empty() ? "" : "; ") + t;
                return {MIXIT_E_NOT_SUPPORTED, "no usable H.264 encoder (" + (all.empty() ? std::string("none in this build") : all) + ")"};
            }
        }
        hardware_ = is_hardware(encoder_name_.c_str());
        if (with_audio) {
            const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (aac == nullptr) return {MIXIT_E_NOT_SUPPORTED, "no AAC encoder in this FFmpeg build"};
            audio_.reset(avcodec_alloc_context3(aac));
            audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
            audio_->sample_rate = static_cast<int>(tl_.sample_rate);
            av_channel_layout_default(&audio_->ch_layout, static_cast<int>(tl_.channels));
            audio_->bit_rate = static_cast<std::int64_t>(key_.audio_kbps) * 1000;
            audio_->time_base = AVRational{1, static_cast<int>(tl_.sample_rate)};
            audio_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            const int err = avcodec_open2(audio_.get(), aac, nullptr);
            if (err < 0) return {MIXIT_E_NOT_SUPPORTED, "AAC encoder: " + av_error_text(err)};
            aframe_ = av_frame_alloc();
            aframe_->format = AV_SAMPLE_FMT_FLTP;
            aframe_->sample_rate = audio_->sample_rate;
            av_channel_layout_copy(&aframe_->ch_layout, &audio_->ch_layout);
            aframe_->nb_samples = audio_->frame_size > 0 ? audio_->frame_size : 1024;
            if (av_frame_get_buffer(aframe_, 0) < 0) return {MIXIT_E_OUT_OF_MEMORY, "audio frame"};
            acc_.assign(tl_.channels, std::vector<float>(static_cast<std::size_t>(aframe_->nb_samples) * 4, 0.0f));
        }
        vframe_ = av_frame_alloc();
        vframe_->format = video_->pix_fmt;
        vframe_->width = static_cast<int>(width_);
        vframe_->height = static_cast<int>(height_);
        if (av_frame_get_buffer(vframe_, 0) < 0) return {MIXIT_E_OUT_OF_MEMORY, "video frame"};
        pkt_ = av_packet_alloc();
        vpar_ = avcodec_parameters_alloc();
        avcodec_parameters_from_context(vpar_, video_.get());
        if (audio_) {
            apar_ = avcodec_parameters_alloc();
            avcodec_parameters_from_context(apar_, audio_.get());
        }
        logger_.logf(MIXIT_LOG_INFO, "output", "encoder {} ({}) {}x{} @ {} kb/s, GOP {} frames{}", encoder_name_,
                     hardware_ ? "hardware" : "software", width_, height_, key_.video_kbps, video_->gop_size,
                     audio_ ? ", AAC " + std::to_string(key_.audio_kbps) + " kb/s" : "");
        thread_ = std::thread([this] { run(); });
        return Status::ok();
    }

    StreamSetup setup() const {
        StreamSetup s;
        s.video = vpar_;
        s.audio = apar_;
        s.video_tb = video_->time_base;
        s.audio_tb = audio_ ? audio_->time_base : AVRational{1, static_cast<int>(tl_.sample_rate)};
        return s;
    }
    [[nodiscard]] const std::string& encoder_name() const noexcept { return encoder_name_; }
    [[nodiscard]] bool hardware() const noexcept { return hardware_; }
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept { return dropped_frames_.load(); }
    void request_keyframe() noexcept { force_key_.store(true); }

    void add_sink(const std::shared_ptr<Sink>& s) {
        std::lock_guard lock(sinks_mutex_);
        sinks_.push_back(s);
    }
    void remove_sink(const Sink* s) {
        std::lock_guard lock(sinks_mutex_);
        std::erase_if(sinks_, [&](const auto& p) { return p.get() == s; });
    }

    // ---- feeding (single producer per kind) ----
    void feed_video(const media::VideoRef& ref, std::uint64_t tick) noexcept {
        const std::uint32_t w = vwrite_.load(std::memory_order_relaxed);
        if (w - vread_.load(std::memory_order_acquire) >= kVideoQueue) {
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        vring_[w % kVideoQueue] = VideoItem{ref, tick};
        vwrite_.store(w + 1, std::memory_order_release);
        wake_.release();
    }
    void feed_audio(const float* samples, std::uint32_t frames, std::int64_t first) noexcept {
        while (frames > 0) {
            const std::uint32_t n = std::min(frames, chunk_frames_);
            const std::uint32_t w = awrite_.load(std::memory_order_relaxed);
            if (w - aread_.load(std::memory_order_acquire) >= kAudioChunks) {
                dropped_audio_.fetch_add(n, std::memory_order_relaxed);
                return;
            }
            const std::uint32_t slot = w % kAudioChunks;
            std::memcpy(chunk_storage_.data() + static_cast<std::size_t>(slot) * chunk_frames_ * tl_.channels, samples,
                        static_cast<std::size_t>(n) * tl_.channels * sizeof(float));
            chunks_[slot] = Chunk{first, n};
            awrite_.store(w + 1, std::memory_order_release);
            samples += static_cast<std::size_t>(n) * tl_.channels;
            first += n;
            frames -= n;
        }
    }

    // Control thread, after the pipeline is no longer fed: encodes what is
    // queued, flushes the encoders and delivers the last packets.
    void finish() noexcept {
        finishing_.store(true);
        wake_.release();
        if (thread_.joinable()) thread_.join();
    }

private:
    struct VideoItem {
        media::VideoRef ref;
        std::uint64_t tick = 0;
    };
    struct Chunk {
        std::int64_t first = 0;
        std::uint32_t frames = 0;
    };

    void deliver(int stream) noexcept {
        std::lock_guard lock(sinks_mutex_);
        for (const auto& s : sinks_) {
            // A private copy per destination: no buffer is shared between the
            // encoder and sink threads (and none outlives a slow sink).
            AVPacket* c = av_packet_alloc();
            if (c == nullptr) continue;
            if (av_new_packet(c, pkt_->size) < 0 || av_packet_copy_props(c, pkt_) < 0) {
                av_packet_free(&c);
                continue;
            }
            std::memcpy(c->data, pkt_->data, static_cast<std::size_t>(pkt_->size));
            s->push(c, stream);
        }
    }

    void drain(AVCodecContext* ctx, int stream) noexcept {
        while (true) {
            const int r = avcodec_receive_packet(ctx, pkt_);
            if (r < 0) break;
            pkt_->stream_index = stream;
            if (stream == 0 && pkt_->dts == AV_NOPTS_VALUE) pkt_->dts = pkt_->pts;
            if (stream == 0) {
                // The first packet is the first frame (an IDR, first in both
                // orders), whose pts we set to 0. Some encoders (Media
                // Foundation) shift timestamps by their reorder delay; undo
                // any constant shift so video stays on the session timeline.
                if (!video_shift_known_) {
                    video_shift_known_ = true;
                    video_shift_ = -pkt_->pts;
                    if (video_shift_ != 0) {
                        logger_.logf(MIXIT_LOG_INFO, "output", "encoder {} shifts timestamps by {} frames; corrected",
                                     encoder_name_, -video_shift_);
                    }
                }
                pkt_->pts += video_shift_;
                pkt_->dts += video_shift_;
            }
            if (stream == 0 && pkt_->duration <= 0) pkt_->duration = 1;  // one tick (time base = 1/fps)
            deliver(stream);
            av_packet_unref(pkt_);
        }
    }

    void encode_video(VideoItem& v) noexcept {
        const media::VideoFrame& f = *v.ref;
        if (!started_) {
            started_ = true;
            start_tick_ = v.tick;
            const std::int64_t t = media::time_of_index(static_cast<std::int64_t>(v.tick), tl_.frame_rate);
            start_sample_ = media::nearest_index(t, media::Rate{tl_.sample_rate, 1});
            next_sample_ = start_sample_;
        }
        sws_ = sws_getCachedContext(sws_, static_cast<int>(f.width), static_cast<int>(f.height), AV_PIX_FMT_BGRA,
                                    static_cast<int>(width_), static_cast<int>(height_), video_->pix_fmt, SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
        if (sws_ == nullptr || av_frame_make_writable(vframe_) < 0) return;
        const int* coefs_src = sws_getCoefficients(SWS_CS_DEFAULT);
        const int* coefs_dst = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(sws_, coefs_src, 1, coefs_dst, 0, 0, 1 << 16, 1 << 16);
        const std::uint8_t* src[1] = {f.data};
        const int src_stride[1] = {static_cast<int>(f.stride)};
        sws_scale(sws_, src, src_stride, 0, static_cast<int>(f.height), vframe_->data, vframe_->linesize);
        v.ref.reset();  // return the frame to the session as early as possible
        vframe_->pts = static_cast<std::int64_t>(v.tick - start_tick_);
        vframe_->pict_type = force_key_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(video_.get(), vframe_) == 0) drain(video_.get(), 0);
    }

    void take_audio(const float* src, std::int64_t first, std::uint32_t frames) noexcept {
        if (!audio_) return;
        std::int64_t skip = next_sample_ - first;  // already covered (overlap)
        if (skip >= frames) return;
        if (skip < 0) {  // gap: insert silence to stay on the timeline
            std::uint32_t gap = static_cast<std::uint32_t>(std::min<std::int64_t>(-skip, tl_.sample_rate));
            while (gap > 0) {
                const std::uint32_t n = std::min(gap, room());
                for (auto& ch : acc_) std::fill_n(ch.begin() + acc_frames_, n, 0.0f);
                acc_frames_ += n;
                gap -= n;
                next_sample_ += n;
                encode_audio_frames();
            }
            next_sample_ = first;
            skip = 0;
        }
        std::uint32_t pos = static_cast<std::uint32_t>(skip);
        while (pos < frames) {
            const std::uint32_t n = std::min(frames - pos, room());
            for (std::uint32_t c = 0; c < tl_.channels; ++c) {
                float* dst = acc_[c].data() + acc_frames_;
                for (std::uint32_t i = 0; i < n; ++i) dst[i] = src[static_cast<std::size_t>(pos + i) * tl_.channels + c];
            }
            acc_frames_ += n;
            pos += n;
            next_sample_ += n;
            encode_audio_frames();
        }
    }
    std::uint32_t room() const noexcept { return static_cast<std::uint32_t>(acc_[0].size()) - acc_frames_; }

    void encode_audio_frames() noexcept {
        const auto fs = static_cast<std::uint32_t>(aframe_->nb_samples);
        while (acc_frames_ >= fs) {
            if (av_frame_make_writable(aframe_) < 0) return;
            for (std::uint32_t c = 0; c < tl_.channels; ++c) {
                std::memcpy(aframe_->data[c], acc_[c].data(), fs * sizeof(float));
                std::memmove(acc_[c].data(), acc_[c].data() + fs, (acc_frames_ - fs) * sizeof(float));
            }
            acc_frames_ -= fs;
            aframe_->pts = audio_pts_;
            audio_pts_ += fs;
            if (avcodec_send_frame(audio_.get(), aframe_) == 0) drain(audio_.get(), 1);
        }
    }

    void pump() noexcept {
        // Video first: it defines the start of the timeline.
        while (vread_.load(std::memory_order_relaxed) != vwrite_.load(std::memory_order_acquire)) {
            const std::uint32_t r = vread_.load(std::memory_order_relaxed);
            VideoItem item = std::move(vring_[r % kVideoQueue]);
            vread_.store(r + 1, std::memory_order_release);
            encode_video(item);
        }
        if (!started_) return;  // keep audio until the first frame sets the start
        while (aread_.load(std::memory_order_relaxed) != awrite_.load(std::memory_order_acquire)) {
            const std::uint32_t r = aread_.load(std::memory_order_relaxed);
            const Chunk c = chunks_[r % kAudioChunks];
            take_audio(chunk_storage_.data() + static_cast<std::size_t>(r % kAudioChunks) * chunk_frames_ * tl_.channels,
                       c.first, c.frames);
            aread_.store(r + 1, std::memory_order_release);
        }
    }

    void run() noexcept {
        os::set_current_thread_name("mixit-enc");
        while (!stop_.load()) {
            (void)wake_.try_acquire_for(std::chrono::milliseconds(20));
            pump();
            if (finishing_.load()) {
                pump();
                // Flush: the encoders hold a few frames (look-ahead, AAC priming).
                if (avcodec_send_frame(video_.get(), nullptr) == 0) drain(video_.get(), 0);
                if (audio_ && acc_frames_ > 0) {  // last partial AAC frame, padded with silence
                    const auto fs = static_cast<std::uint32_t>(aframe_->nb_samples);
                    for (auto& ch : acc_) std::fill(ch.begin() + acc_frames_, ch.begin() + fs, 0.0f);
                    acc_frames_ = fs;
                    encode_audio_frames();
                }
                if (audio_ && avcodec_send_frame(audio_.get(), nullptr) == 0) drain(audio_.get(), 1);
                break;
            }
        }
    }

    const PipelineKey key_;
    const Timeline tl_;
    Logger& logger_;
    std::uint32_t width_ = 0, height_ = 0;
    std::string encoder_name_;
    bool hardware_ = false;
    CodecCtxPtr video_;
    CodecCtxPtr audio_;
    AVCodecParameters* vpar_ = nullptr;
    AVCodecParameters* apar_ = nullptr;
    AVFrame* vframe_ = nullptr;
    AVFrame* aframe_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;

    // Encoder thread state.
    bool started_ = false;
    std::uint64_t start_tick_ = 0;
    std::int64_t start_sample_ = 0;
    std::int64_t next_sample_ = 0;
    std::int64_t audio_pts_ = 0;
    std::vector<std::vector<float>> acc_;
    std::uint32_t acc_frames_ = 0;
    bool video_shift_known_ = false;
    std::int64_t video_shift_ = 0;

    // Feeding rings (SPSC).
    std::array<VideoItem, kVideoQueue> vring_{};
    std::atomic<std::uint32_t> vwrite_{0}, vread_{0};
    std::uint32_t chunk_frames_ = 480;
    std::vector<float> chunk_storage_;
    std::array<Chunk, kAudioChunks> chunks_{};
    std::atomic<std::uint32_t> awrite_{0}, aread_{0};
    std::atomic<std::uint64_t> dropped_frames_{0}, dropped_audio_{0};
    std::atomic<bool> force_key_{false};

    std::counting_semaphore<1 << 20> wake_{0};
    std::atomic<bool> stop_{false}, finishing_{false};
    std::thread thread_;
    std::mutex sinks_mutex_;
    std::vector<std::shared_ptr<Sink>> sinks_;
};

// ---------------------------------------------------------------------------------- manager

struct OutputManager::Output {
    std::uint32_t id = 0;
    OutputConfig config;
    std::string format;
    std::shared_ptr<Sink> sink;
    std::shared_ptr<EncoderPipeline> pipeline;
    OutputStats last;  // kept after the sink is gone
    std::uint64_t encoder_drops = 0;  // of the encoder it last used
};

OutputManager::OutputManager(const Timeline& timeline, Logger& logger, std::function<void()> wait_quiescent)
    : timeline_(timeline), logger_(logger), wait_quiescent_(std::move(wait_quiescent)) {}

OutputManager::~OutputManager() { stop_all(); }

Status OutputManager::add(const OutputConfig& config, std::uint32_t& out_id) {
    if (!media::FfmpegLibrary::info().usable()) return {MIXIT_E_NOT_SUPPORTED, "outputs need FFmpeg (not available)"};
    if (config.url.empty()) return {MIXIT_E_INVALID_ARGUMENT, "url is empty"};
    if (config.video_kbps < 100 || config.video_kbps > 200'000) return {MIXIT_E_INVALID_ARGUMENT, "video_kbps must be 100..200000"};
    if (config.audio_kbps < 32 || config.audio_kbps > 512) return {MIXIT_E_INVALID_ARGUMENT, "audio_kbps must be 32..512"};
    if (config.keyframe_ms < 250 || config.keyframe_ms > 10'000) return {MIXIT_E_INVALID_ARGUMENT, "keyframe_ms must be 250..10000"};
    if (config.segment_seconds != 0 && (config.segment_seconds < 2 || is_network_url(config.url))) {
        return {MIXIT_E_INVALID_ARGUMENT, "segment_seconds must be 0 or >= 2, files only"};
    }
    std::string format;
    if (Status st = resolve_format(config, format); !st.is_ok()) return st;
    install_log_redaction();
    std::lock_guard lock(mutex_);
    auto o = std::make_unique<Output>();
    o->id = next_id_++;
    o->config = config;
    o->format = format;
    out_id = o->id;
    outputs_.push_back(std::move(o));
    return Status::ok();
}

Status OutputManager::remove(std::uint32_t id) {
    std::lock_guard lock(mutex_);
    reap_locked();
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->id == id; });
    if (it == outputs_.end()) return {MIXIT_E_NOT_FOUND, "no output " + std::to_string(id)};
    if ((*it)->sink) return {MIXIT_E_INVALID_STATE, "stop the output first"};
    outputs_.erase(it);
    return Status::ok();
}

Status OutputManager::start(std::uint32_t id, std::uint32_t width, std::uint32_t height, bool has_audio) {
    std::lock_guard lock(mutex_);
    reap_locked();
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->id == id; });
    if (it == outputs_.end()) return {MIXIT_E_NOT_FOUND, "no output " + std::to_string(id)};
    Output& o = **it;
    if (o.sink) return {MIXIT_E_INVALID_STATE, "output already started"};
    PipelineKey key{o.config.source_input, o.config.video_encoder, o.config.video_kbps, o.config.keyframe_ms,
                    o.config.audio_kbps};
    std::shared_ptr<EncoderPipeline> pipeline;
    for (const auto& p : pipelines_) {
        if (p->key() == key) pipeline = p;
    }
    if (!pipeline) {
        std::size_t free_slot = kMaxPipelines;
        for (std::size_t i = 0; i < kMaxPipelines; ++i) {
            if (slots_[i].load() == nullptr) {
                free_slot = i;
                break;
            }
        }
        if (free_slot == kMaxPipelines) return {MIXIT_E_INVALID_STATE, "too many different encodes at once"};
        pipeline = std::make_shared<EncoderPipeline>(key, timeline_, logger_);
        if (Status st = pipeline->open(width, height, has_audio); !st.is_ok()) {
            logger_.logf(MIXIT_LOG_ERROR, "output", "output {}: {}", id, st.message());
            o.last.state = OutputState::Failed;
            o.last.last_error = st.message();
            return st;
        }
        pipelines_.push_back(pipeline);
        slots_[free_slot].store(pipeline.get(), std::memory_order_release);
        refresh_mask();
    }
    std::weak_ptr<EncoderPipeline> weak = pipeline;
    o.sink = std::make_shared<Sink>(o.id, o.config, o.format, logger_, [weak] {
        if (auto p = weak.lock()) p->request_keyframe();
    });
    o.sink->start(pipeline->setup(), pipeline->encoder_name(), pipeline->hardware());
    o.pipeline = pipeline;
    pipeline->add_sink(o.sink);
    o.last = OutputStats{};
    return Status::ok();
}

// Releases the output's use of its encoder. The last user unpublishes the
// pipeline, waits until the session threads cannot touch it, and flushes it
// so the stopping destination still gets the final packets.
void OutputManager::release_pipeline_locked(Output& o) noexcept {
    std::shared_ptr<EncoderPipeline> p = std::move(o.pipeline);
    if (!p) return;
    o.encoder_drops = p->dropped_frames();
    const bool last_user = std::none_of(outputs_.begin(), outputs_.end(),
                                        [&](const auto& other) { return other.get() != &o && other->pipeline == p; });
    if (last_user) {
        for (auto& slot : slots_) {
            if (slot.load() == p.get()) slot.store(nullptr, std::memory_order_seq_cst);
        }
        refresh_mask();
        if (wait_quiescent_) wait_quiescent_();
        p->finish();
        std::erase(pipelines_, p);
    }
    if (o.sink) p->remove_sink(o.sink.get());
}

void OutputManager::detach_locked(Output& o) noexcept {
    if (!o.sink) return;
    release_pipeline_locked(o);
    o.sink->request_stop();
}

Status OutputManager::stop(std::uint32_t id) {
    std::lock_guard lock(mutex_);
    reap_locked();
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->id == id; });
    if (it == outputs_.end()) return {MIXIT_E_NOT_FOUND, "no output " + std::to_string(id)};
    if (!(*it)->sink) return Status::ok();
    detach_locked(**it);
    return Status::ok();
}

void OutputManager::stop_all() noexcept {
    std::vector<std::shared_ptr<Sink>> sinks;
    {
        std::lock_guard lock(mutex_);
        for (auto& o : outputs_) {
            if (o->sink) {
                detach_locked(*o);
                sinks.push_back(o->sink);
            }
        }
    }
    for (auto& s : sinks) s->join();  // files finalized here
    std::lock_guard lock(mutex_);
    reap_locked();
}

// Keeps the final stats of finished destinations (stopped or failed on
// their own) and releases them and their encoder.
void OutputManager::reap_locked() noexcept {
    for (auto& o : outputs_) {
        if (o->sink && o->sink->finished()) {
            release_pipeline_locked(*o);
            o->sink->join();
            o->last = o->sink->stats();
            o->sink.reset();
        }
    }
}

Status OutputManager::stats(std::uint32_t id, OutputStats& out) const {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->id == id; });
    if (it == outputs_.end()) return {MIXIT_E_NOT_FOUND, "no output " + std::to_string(id)};
    const Output& o = **it;
    out = o.sink ? o.sink->stats() : o.last;
    out.encoder_dropped_frames = o.pipeline ? o.pipeline->dropped_frames() : o.encoder_drops;
    return Status::ok();
}

Status OutputManager::config(std::uint32_t id, OutputConfig& out) const {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->id == id; });
    if (it == outputs_.end()) return {MIXIT_E_NOT_FOUND, "no output " + std::to_string(id)};
    out = (*it)->config;
    return Status::ok();
}

std::vector<std::uint32_t> OutputManager::ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint32_t> v;
    for (const auto& o : outputs_) v.push_back(o->id);
    return v;
}

bool OutputManager::has_source(std::uint32_t source) const {
    std::lock_guard lock(mutex_);
    return std::any_of(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o->config.source_input == source; });
}

void OutputManager::refresh_mask() noexcept {
    std::uint64_t mask = 0;
    for (const auto& slot : slots_) {
        if (const EncoderPipeline* p = slot.load()) mask |= mask_bit(p->key().source);
    }
    source_mask_.store(mask, std::memory_order_release);
}

void OutputManager::feed_video(std::uint32_t source, const media::VideoRef& frame, std::uint64_t tick) noexcept {
    for (auto& slot : slots_) {
        if (EncoderPipeline* p = slot.load(std::memory_order_acquire); p != nullptr && p->key().source == source) {
            p->feed_video(frame, tick);
        }
    }
}

void OutputManager::feed_audio(std::uint32_t source, const float* samples, std::uint32_t frames,
                               std::int64_t first_sample) noexcept {
    for (auto& slot : slots_) {
        if (EncoderPipeline* p = slot.load(std::memory_order_acquire); p != nullptr && p->key().source == source) {
            p->feed_audio(samples, frames, first_sample);
        }
    }
}

}  // namespace mixit::output

#else  // no FFmpeg

namespace mixit::output {

class EncoderPipeline {};
class Sink {};
struct OutputManager::Output {};

Status query_video_encoders(std::vector<EncoderInfo>& out) {
    out.clear();
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without FFmpeg"};
}
OutputManager::OutputManager(const Timeline& timeline, Logger& logger, std::function<void()> wait_quiescent)
    : timeline_(timeline), logger_(logger), wait_quiescent_(std::move(wait_quiescent)) {}
OutputManager::~OutputManager() = default;
Status OutputManager::add(const OutputConfig&, std::uint32_t&) { return {MIXIT_E_NOT_SUPPORTED, "outputs need FFmpeg"}; }
Status OutputManager::remove(std::uint32_t) { return {MIXIT_E_NOT_FOUND, "no such output"}; }
Status OutputManager::start(std::uint32_t, std::uint32_t, std::uint32_t, bool) { return {MIXIT_E_NOT_FOUND, "no such output"}; }
Status OutputManager::stop(std::uint32_t) { return {MIXIT_E_NOT_FOUND, "no such output"}; }
void OutputManager::stop_all() noexcept {}
Status OutputManager::stats(std::uint32_t, OutputStats&) const { return {MIXIT_E_NOT_FOUND, "no such output"}; }
Status OutputManager::config(std::uint32_t, OutputConfig&) const { return {MIXIT_E_NOT_FOUND, "no such output"}; }
std::vector<std::uint32_t> OutputManager::ids() const { return {}; }
bool OutputManager::has_source(std::uint32_t) const { return false; }
void OutputManager::detach_locked(Output&) noexcept {}
void OutputManager::release_pipeline_locked(Output&) noexcept {}
void OutputManager::refresh_mask() noexcept {}
void OutputManager::reap_locked() noexcept {}
void OutputManager::feed_video(std::uint32_t, const media::VideoRef&, std::uint64_t) noexcept {}
void OutputManager::feed_audio(std::uint32_t, const float*, std::uint32_t, std::int64_t) noexcept {}

}  // namespace mixit::output

#endif
