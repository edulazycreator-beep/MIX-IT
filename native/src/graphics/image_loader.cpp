#include "graphics/image_loader.hpp"

#if MIXIT_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "media/ffmpeg_library.hpp"

namespace mixit::graphics {

Status load_image_bgra(const std::string& path, Image& out) {
    if (!media::FfmpegLibrary::info().usable()) return {MIXIT_E_NOT_SUPPORTED, "images need FFmpeg (not available)"};
    AVFormatContext* fc = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);  // local files only
    int err = avformat_open_input(&fc, path.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err < 0) return {MIXIT_E_NOT_FOUND, "cannot open"};
    struct Close {
        AVFormatContext*& f;
        ~Close() { avformat_close_input(&f); }
    } close_fc{fc};
    if (avformat_find_stream_info(fc, nullptr) < 0) return {MIXIT_E_INVALID_ARGUMENT, "not an image"};
    const int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) return {MIXIT_E_INVALID_ARGUMENT, "not an image"};
    const AVCodecParameters* par = fc->streams[vi]->codecpar;
    if (par->width <= 0 || par->height <= 0 || par->width > 16384 || par->height > 16384) {
        return {MIXIT_E_INVALID_ARGUMENT, "image size out of range"};
    }
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (codec == nullptr) return {MIXIT_E_NOT_SUPPORTED, "no decoder for this image format"};
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    struct FreeCtx {
        AVCodecContext*& c;
        ~FreeCtx() { avcodec_free_context(&c); }
    } free_ctx{ctx};
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, nullptr) < 0) return {MIXIT_E_NOT_SUPPORTED, "cannot open the image decoder"};
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vi && avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, frame) >= 0) got = true;
        av_packet_unref(pkt);
    }
    if (!got && avcodec_send_packet(ctx, nullptr) >= 0 && avcodec_receive_frame(ctx, frame) >= 0) got = true;
    Status result = got ? Status::ok() : Status{MIXIT_E_INVALID_ARGUMENT, "could not decode the image"};
    if (got) {
        SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                                         frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) {
            result = {MIXIT_E_NOT_SUPPORTED, "unsupported pixel format"};
        } else {
            out.width = static_cast<std::uint32_t>(frame->width);
            out.height = static_cast<std::uint32_t>(frame->height);
            out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
            std::uint8_t* dst[1] = {out.pixels.data()};
            const int dst_stride[1] = {static_cast<int>(out.width * 4)};
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
            sws_freeContext(sws);
            // Formats without alpha come out with alpha 0 from some swscale
            // paths: make them opaque.
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) == 0) {
                for (std::size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 255;
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return result;
}

}  // namespace mixit::graphics

#else

namespace mixit::graphics {
Status load_image_bgra(const std::string&, Image&) { return {MIXIT_E_NOT_SUPPORTED, "images need FFmpeg"}; }
}  // namespace mixit::graphics

#endif
