#include "media_file_reader.hpp"

#if MIXIT_HAVE_FFMPEG

#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

namespace mixit::test {

namespace {

struct Deadline {
    std::chrono::steady_clock::time_point until;
};

int interrupt(void* opaque) {
    return std::chrono::steady_clock::now() > static_cast<Deadline*>(opaque)->until ? 1 : 0;
}

}  // namespace

DecodedMedia decode_media(const std::string& url, const std::string& options, double max_seconds, double timeout_seconds) {
    DecodedMedia out;
    Deadline deadline{std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(timeout_seconds * 1000))};
    AVFormatContext* fc = avformat_alloc_context();
    fc->interrupt_callback = AVIOInterruptCB{&interrupt, &deadline};
    AVDictionary* opts = nullptr;
    std::size_t pos = 0;
    while (pos < options.size()) {
        std::size_t end = options.find(';', pos);
        if (end == std::string::npos) end = options.size();
        const std::string kv = options.substr(pos, end - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) av_dict_set(&opts, kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 0);
        pos = end + 1;
    }
    int err = avformat_open_input(&fc, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        char buf[128];
        av_strerror(err, buf, sizeof(buf));
        out.error = std::string("open: ") + buf;
        return out;
    }
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        out.error = "find_stream_info failed";
        avformat_close_input(&fc);
        return out;
    }
    out.format = fc->iformat->name;
    const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ++out.video_streams;
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++out.audio_streams;
    }
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    auto open_dec = [&](int index) -> AVCodecContext* {
        const AVCodecParameters* par = fc->streams[index]->codecpar;
        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (c == nullptr) return nullptr;
        AVCodecContext* ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(ctx, par);
        if (avcodec_open2(ctx, c, nullptr) < 0) avcodec_free_context(&ctx);
        return ctx;
    };
    if (vi >= 0) {
        vdec = open_dec(vi);
        out.video_codec = avcodec_get_name(fc->streams[vi]->codecpar->codec_id);
        out.width = fc->streams[vi]->codecpar->width;
        out.height = fc->streams[vi]->codecpar->height;
    }
    if (ai >= 0) {
        adec = open_dec(ai);
        out.audio_codec = avcodec_get_name(fc->streams[ai]->codecpar->codec_id);
        out.sample_rate = fc->streams[ai]->codecpar->sample_rate;
        out.channels = fc->streams[ai]->codecpar->ch_layout.nb_channels;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool audio_started = false;
    auto take = [&](AVCodecContext* dec, int stream) {
        while (avcodec_receive_frame(dec, frame) >= 0) {
            const AVRational tb = fc->streams[stream]->time_base;
            const std::int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const double t = ts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(ts) * av_q2d(tb);
            if (stream == vi) {
                out.frame_times.push_back(t);
                double sum = 0;
                int n = 0;
                for (int y = frame->height - frame->height / 8 + 2; y < frame->height - 2; ++y) {
                    const std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
                    for (int x = 0; x < frame->width; x += 4) {
                        sum += row[x];
                        ++n;
                    }
                }
                out.bottom_luma.push_back(n > 0 ? sum / n : 0.0);
            } else {
                if (!audio_started) {
                    audio_started = true;
                    out.audio_start = t;
                }
                if (frame->format == AV_SAMPLE_FMT_FLTP || frame->format == AV_SAMPLE_FMT_FLT) {
                    const auto* d = reinterpret_cast<const float*>(frame->data[0]);
                    const int step = frame->format == AV_SAMPLE_FMT_FLT ? frame->ch_layout.nb_channels : 1;
                    for (int i = 0; i < frame->nb_samples; ++i) out.audio.push_back(d[i * step]);
                }
            }
        }
    };
    while (true) {
        deadline.until = std::max(deadline.until, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        err = av_read_frame(fc, pkt);
        if (err < 0) break;
        if (pkt->stream_index == vi && vdec != nullptr) {
            if (avcodec_send_packet(vdec, pkt) < 0) ++out.decode_errors;
            take(vdec, vi);
        } else if (pkt->stream_index == ai && adec != nullptr) {
            if (avcodec_send_packet(adec, pkt) < 0) ++out.decode_errors;
            take(adec, ai);
        }
        av_packet_unref(pkt);
        if (max_seconds > 0 && !out.frame_times.empty() && out.frame_times.back() - out.frame_times.front() >= max_seconds) break;
    }
    if (vdec != nullptr) {
        avcodec_send_packet(vdec, nullptr);
        take(vdec, vi);
    }
    if (adec != nullptr) {
        avcodec_send_packet(adec, nullptr);
        take(adec, ai);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fc);
    return out;
}

}  // namespace mixit::test

#else

namespace mixit::test {
DecodedMedia decode_media(const std::string&, const std::string&, double, double) {
    DecodedMedia m;
    m.error = "built without FFmpeg";
    return m;
}
}  // namespace mixit::test

#endif
