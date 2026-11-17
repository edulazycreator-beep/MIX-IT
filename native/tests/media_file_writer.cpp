#include "media_file_writer.hpp"

#if MIXIT_HAVE_FFMPEG

#include <cstring>
#include <vector>

#include "media/media_types.hpp"
#include "media/test_signal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

namespace mixit::testing {
namespace {

std::string err_text(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

struct Output {
    AVFormatContext* oc = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    AVPacket* pkt = av_packet_alloc();
    ~Output() {
        avcodec_free_context(&venc);
        avcodec_free_context(&aenc);
        av_packet_free(&pkt);
        if (oc != nullptr) {
            if (oc->pb != nullptr) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
    std::string drain(AVCodecContext* enc, AVStream* st) {
        for (;;) {
            const int r = avcodec_receive_packet(enc, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return {};
            if (r < 0) return "receive_packet: " + err_text(r);
            // Some encoders leave packet durations unset (FFmpeg 9 mpeg4);
            // the MP4 muxer would then write a zero-length last sample that
            // the edit list excludes, silently dropping the final frame.
            if (pkt->duration == 0 && enc->codec_type == AVMEDIA_TYPE_VIDEO) pkt->duration = 1;
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (const int w = av_interleaved_write_frame(oc, pkt); w < 0) return "write: " + err_text(w);
        }
    }
};

}  // namespace

std::string write_test_media(const TestMediaSpec& spec) {
    Output o;
    if (const int e = avformat_alloc_output_context2(&o.oc, nullptr, spec.container.c_str(), spec.path.c_str()); e < 0) {
        return "alloc output: " + err_text(e);
    }
    const bool global_header = (o.oc->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    if (spec.video) {
        const AVCodec* codec = avcodec_find_encoder(spec.compressed ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_RAWVIDEO);
        if (codec == nullptr) return "video encoder not available";
        o.venc = avcodec_alloc_context3(codec);
        o.venc->width = static_cast<int>(spec.width);
        o.venc->height = static_cast<int>(spec.height);
        o.venc->time_base = {static_cast<int>(spec.frame_rate.den), static_cast<int>(spec.frame_rate.num)};
        o.venc->framerate = {static_cast<int>(spec.frame_rate.num), static_cast<int>(spec.frame_rate.den)};
        o.venc->pix_fmt = spec.compressed ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_BGRA;
        if (spec.compressed) {
            o.venc->gop_size = 12;
            o.venc->flags |= AV_CODEC_FLAG_QSCALE;
            o.venc->global_quality = FF_QP2LAMBDA * 2;  // near lossless: frame codes must survive
            o.venc->colorspace = AVCOL_SPC_BT709;
            o.venc->color_range = AVCOL_RANGE_MPEG;
        }
        if (global_header) o.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (const int e = avcodec_open2(o.venc, codec, nullptr); e < 0) return "open video encoder: " + err_text(e);
        o.vst = avformat_new_stream(o.oc, nullptr);
        o.vst->time_base = o.venc->time_base;
        avcodec_parameters_from_context(o.vst->codecpar, o.venc);
    }
    if (spec.audio) {
        const AVCodec* codec = avcodec_find_encoder(spec.compressed ? AV_CODEC_ID_AAC : AV_CODEC_ID_PCM_F32LE);
        if (codec == nullptr) return "audio encoder not available";
        o.aenc = avcodec_alloc_context3(codec);
        o.aenc->sample_rate = static_cast<int>(spec.sample_rate);
        av_channel_layout_default(&o.aenc->ch_layout, static_cast<int>(spec.channels));
        o.aenc->sample_fmt = spec.compressed ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
        o.aenc->time_base = {1, static_cast<int>(spec.sample_rate)};
        if (spec.compressed) o.aenc->bit_rate = 256000;
        if (global_header) o.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (const int e = avcodec_open2(o.aenc, codec, nullptr); e < 0) return "open audio encoder: " + err_text(e);
        o.ast = avformat_new_stream(o.oc, nullptr);
        o.ast->time_base = o.aenc->time_base;
        avcodec_parameters_from_context(o.ast->codecpar, o.aenc);
    }
    if (const int e = avio_open(&o.oc->pb, spec.path.c_str(), AVIO_FLAG_WRITE); e < 0) return "avio_open: " + err_text(e);
    if (const int e = avformat_write_header(o.oc, nullptr); e < 0) return "write header: " + err_text(e);

    media::TestSignalConfig tc;
    tc.width = spec.width;
    tc.height = spec.height;
    tc.frame_rate = spec.frame_rate;
    tc.sample_rate = spec.sample_rate;
    tc.channels = spec.channels;
    tc.flash_interval_frames = spec.flash_interval_frames;
    tc.source_id = 1;
    const media::TestSignal signal(tc);

    // Source pixels / samples rendered with the engine's own test signal.
    const std::uint32_t stride = media::aligned_stride(spec.width, 4);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * spec.height);
    media::VideoFrame vf;
    vf.width = spec.width;
    vf.height = spec.height;
    vf.stride = stride;
    vf.data = pixels.data();

    const int audio_frame_size = (o.aenc != nullptr && o.aenc->frame_size > 0) ? o.aenc->frame_size : 480;
    std::vector<float> interleaved(static_cast<std::size_t>(audio_frame_size) * spec.channels);
    media::AudioBlock ab;
    ab.channels = spec.channels;
    ab.capacity_frames = static_cast<std::uint32_t>(audio_frame_size);
    ab.samples = interleaved.data();

    SwsContext* sws = nullptr;
    AVFrame* frame = av_frame_alloc();
    const std::int64_t total_samples =
        media::muldiv_i64(media::time_of_index(spec.frames, spec.frame_rate), spec.sample_rate, media::kNanosPerSecond)
            .value_or(0);
    std::int64_t next_frame = spec.video ? 0 : spec.frames;
    std::int64_t next_sample = spec.audio ? 0 : total_samples;
    std::string error;

    while (error.empty() && (next_frame < spec.frames || next_sample < total_samples)) {
        const std::int64_t vt = next_frame < spec.frames ? media::time_of_index(next_frame, spec.frame_rate) : INT64_MAX;
        const std::int64_t at =
            next_sample < total_samples ? media::time_of_index(next_sample, media::Rate{spec.sample_rate, 1}) : INT64_MAX;
        av_frame_unref(frame);
        if (vt <= at) {
            signal.render_video(vf, next_frame, 0);
            frame->format = o.venc->pix_fmt;
            frame->width = o.venc->width;
            frame->height = o.venc->height;
            av_frame_get_buffer(frame, 0);
            if (o.venc->pix_fmt == AV_PIX_FMT_BGRA) {
                for (std::uint32_t y = 0; y < spec.height; ++y) {
                    std::memcpy(frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0],
                                pixels.data() + static_cast<std::size_t>(y) * stride, spec.width * 4);
                }
            } else {
                sws = sws_getCachedContext(sws, o.venc->width, o.venc->height, AV_PIX_FMT_BGRA, o.venc->width,
                                           o.venc->height, o.venc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
                sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709),
                                         0, 0, 1 << 16, 1 << 16);
                const std::uint8_t* src[1] = {pixels.data()};
                const int src_stride[1] = {static_cast<int>(stride)};
                sws_scale(sws, src, src_stride, 0, o.venc->height, frame->data, frame->linesize);
            }
            frame->pts = next_frame++;
            frame->duration = 1;
            if (avcodec_send_frame(o.venc, frame) < 0) error = "send video frame";
            else error = o.drain(o.venc, o.vst);
        } else {
            const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(audio_frame_size, total_samples - next_sample));
            signal.render_audio(ab, next_sample, n, 0);
            frame->format = o.aenc->sample_fmt;
            frame->nb_samples = static_cast<int>(n);
            av_channel_layout_copy(&frame->ch_layout, &o.aenc->ch_layout);
            frame->sample_rate = o.aenc->sample_rate;
            av_frame_get_buffer(frame, 0);
            for (std::uint32_t i = 0; i < n; ++i) {
                for (std::uint32_t c = 0; c < spec.channels; ++c) {
                    const float v = interleaved[static_cast<std::size_t>(i) * spec.channels + c];
                    if (o.aenc->sample_fmt == AV_SAMPLE_FMT_FLT) {
                        reinterpret_cast<float*>(frame->data[0])[static_cast<std::size_t>(i) * spec.channels + c] = v;
                    } else {
                        reinterpret_cast<float*>(frame->data[c])[i] = v;
                    }
                }
            }
            frame->pts = next_sample;
            frame->duration = n;
            next_sample += n;
            if (avcodec_send_frame(o.aenc, frame) < 0) error = "send audio frame";
            else error = o.drain(o.aenc, o.ast);
        }
    }
    if (error.empty() && o.venc != nullptr) {
        avcodec_send_frame(o.venc, nullptr);
        error = o.drain(o.venc, o.vst);
    }
    if (error.empty() && o.aenc != nullptr) {
        avcodec_send_frame(o.aenc, nullptr);
        error = o.drain(o.aenc, o.ast);
    }
    av_frame_free(&frame);
    sws_freeContext(sws);
    if (error.empty()) {
        if (const int e = av_write_trailer(o.oc); e < 0) error = "write trailer: " + err_text(e);
    }
    return error;
}

}  // namespace mixit::testing

#endif
