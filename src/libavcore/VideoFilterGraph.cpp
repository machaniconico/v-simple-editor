#include "VideoFilterGraph.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace libavcore {

namespace {

struct InputFormatGuard {
    AVFormatContext* ctx = nullptr;

    ~InputFormatGuard()
    {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

struct OutputFormatGuard {
    AVFormatContext* ctx = nullptr;

    ~OutputFormatGuard()
    {
        if (ctx) {
            if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) {
                avio_closep(&ctx->pb);
            }
            avformat_free_context(ctx);
        }
    }
};

struct CodecContextGuard {
    AVCodecContext* ctx = nullptr;

    ~CodecContextGuard()
    {
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};

struct FilterGraphGuard {
    AVFilterGraph* graph = nullptr;

    ~FilterGraphGuard()
    {
        if (graph) {
            avfilter_graph_free(&graph);
        }
    }
};

struct PacketGuard {
    AVPacket* pkt = nullptr;

    PacketGuard()
        : pkt(av_packet_alloc())
    {
    }

    ~PacketGuard()
    {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
};

struct FrameGuard {
    AVFrame* frame = nullptr;

    FrameGuard()
        : frame(av_frame_alloc())
    {
    }

    ~FrameGuard()
    {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

struct InOutGuard {
    AVFilterInOut* io = nullptr;

    explicit InOutGuard(AVFilterInOut* value)
        : io(value)
    {
    }

    ~InOutGuard()
    {
        if (io) {
            avfilter_inout_free(&io);
        }
    }
};

struct SwsGuard {
    SwsContext* ctx = nullptr;

    ~SwsGuard()
    {
        if (ctx) {
            sws_freeContext(ctx);
        }
    }
};

struct DictGuard {
    AVDictionary* dict = nullptr;

    ~DictGuard()
    {
        if (dict) {
            av_dict_free(&dict);
        }
    }
};

std::string ffmpegErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return std::to_string(err);
    }
    return std::string(buf);
}

bool validRational(AVRational value)
{
    return value.num > 0 && value.den > 0;
}

std::string codecNameFor(const AVCodecParameters* par)
{
    if (!par) return std::string();
    const char* name = avcodec_get_name(par->codec_id);
    return name ? std::string(name) : std::string();
}

std::string firstFilterName(const std::string& description)
{
    size_t i = 0;
    const size_t n = description.size();

    while (i < n && std::isspace(static_cast<unsigned char>(description[i]))) {
        ++i;
    }
    while (i < n && description[i] == '[') {
        const size_t end = description.find(']', i + 1);
        if (end == std::string::npos) {
            return std::string();
        }
        i = end + 1;
        while (i < n && std::isspace(static_cast<unsigned char>(description[i]))) {
            ++i;
        }
    }

    const size_t start = i;
    while (i < n) {
        const char ch = description[i];
        if (ch == '=' || ch == ',' || ch == ';' || ch == '['
            || std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        ++i;
    }

    std::string name = description.substr(start, i - start);
    const size_t instanceMarker = name.find('@');
    if (instanceMarker != std::string::npos) {
        name.resize(instanceMarker);
    }
    return name;
}

int64_t bestDurationUs(const AVFormatContext* fmt, const AVStream* videoStream)
{
    if (fmt && fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        return fmt->duration;
    }
    if (videoStream && videoStream->duration != AV_NOPTS_VALUE
        && videoStream->duration > 0 && validRational(videoStream->time_base)) {
        return av_rescale_q(videoStream->duration, videoStream->time_base,
                            AVRational{1, AV_TIME_BASE});
    }
    return 1;
}

AVRational bestFrameRate(AVFormatContext* fmt, AVStream* stream)
{
    if (!stream) return AVRational{0, 1};

    const AVRational guessed = av_guess_frame_rate(fmt, stream, nullptr);
    if (validRational(guessed)) return guessed;
    if (validRational(stream->avg_frame_rate)) return stream->avg_frame_rate;
    if (validRational(stream->r_frame_rate)) return stream->r_frame_rate;
    return AVRational{25, 1};
}

const AVPixelFormat* querySupportedPixelFormats(const AVCodec* encoder)
{
    if (!encoder) return nullptr;

    const void* cfg = nullptr;
    int numCfg = 0;
    const int rc = avcodec_get_supported_config(
        nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &numCfg);
    if (rc >= 0 && cfg && numCfg > 0) {
        const AVPixelFormat* src = static_cast<const AVPixelFormat*>(cfg);
        AVPixelFormat* out = static_cast<AVPixelFormat*>(
            av_malloc(sizeof(AVPixelFormat) * (static_cast<size_t>(numCfg) + 1)));
        if (!out) return nullptr;
        for (int i = 0; i < numCfg; ++i) out[i] = src[i];
        out[numCfg] = AV_PIX_FMT_NONE;
        return out;
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const AVPixelFormat* legacy = encoder->pix_fmts;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!legacy) return nullptr;

    int n = 0;
    while (legacy[n] != AV_PIX_FMT_NONE) ++n;
    AVPixelFormat* out = static_cast<AVPixelFormat*>(
        av_malloc(sizeof(AVPixelFormat) * (static_cast<size_t>(n) + 1)));
    if (!out) return nullptr;
    for (int i = 0; i <= n; ++i) out[i] = legacy[i];
    return out;
}

bool isSoftwarePixelFormat(AVPixelFormat fmt)
{
    if (fmt == AV_PIX_FMT_NONE) return false;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    return desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

AVPixelFormat chooseEncoderPixelFormat(const AVCodec* encoder,
                                       AVPixelFormat filteredFormat)
{
    if (!encoder) return AV_PIX_FMT_YUV420P;

    const AVPixelFormat* list = querySupportedPixelFormats(encoder);
    if (!list) {
        return filteredFormat == AV_PIX_FMT_NONE
                   ? AV_PIX_FMT_YUV420P
                   : filteredFormat;
    }

    AVPixelFormat firstSoftware = AV_PIX_FMT_NONE;
    bool supportsFiltered = false;
    for (const AVPixelFormat* p = list; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == filteredFormat && isSoftwarePixelFormat(*p)) {
            supportsFiltered = true;
            break;
        }
        if (firstSoftware == AV_PIX_FMT_NONE && isSoftwarePixelFormat(*p)) {
            firstSoftware = *p;
        }
    }
    av_free(const_cast<AVPixelFormat*>(list));

    if (supportsFiltered) return filteredFormat;
    if (firstSoftware != AV_PIX_FMT_NONE) return firstSoftware;
    return AV_PIX_FMT_YUV420P;
}

// Fallback name chain that mirrors libavcore::FrameEncoder's
// appendOrderedFamilyFallbacks() for H264. We prefer software / Media
// Foundation encoders that accept yuv420p over GPU encoders (h264_d3d12va,
// h264_qsv, ...) whose required pix_fmt is a hardware-frame format. Using
// avcodec_find_encoder(AV_CODEC_ID_H264) alone hits whichever H264 codec is
// registered first on Windows — h264_d3d12va in the bundled DLL — and the
// d3d12 pix_fmt requirement breaks the buffersink→encoder handoff.
static const AVCodec* findFirstAvailableH264Software()
{
    static const char* kCandidates[] = {
        "libx264", "h264_mf", "h264_nvenc", "h264_qsv", "h264_amf", "mpeg4"
    };
    for (const char* name : kCandidates) {
        if (const AVCodec* c = avcodec_find_encoder_by_name(name)) return c;
    }
    return avcodec_find_encoder(AV_CODEC_ID_H264);
}

const AVCodec* resolveEncoder(const VideoFilterRequest& req,
                              AVCodecID inputCodecId)
{
    if (!req.videoCodecName.empty()) {
        const AVCodec* named =
            avcodec_find_encoder_by_name(req.videoCodecName.c_str());
        if (named) return named;
        return findFirstAvailableH264Software();
    }

    const AVCodec* byInput = avcodec_find_encoder(inputCodecId);
    if (byInput) return byInput;
    return findFirstAvailableH264Software();
}

std::optional<std::string> drainEncoder(AVCodecContext* encCtx,
                                        AVStream* outVideoStream,
                                        AVFormatContext* outFmt,
                                        AVPacket* encPkt)
{
    while (true) {
        int rc = avcodec_receive_packet(encCtx, encPkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return std::nullopt;
        }
        if (rc < 0) {
            av_packet_unref(encPkt);
            return std::string("VideoFilterGraph: failed to receive encoded "
                               "video packet: ") + ffmpegErrorString(rc);
        }

        encPkt->stream_index = outVideoStream->index;
        av_packet_rescale_ts(encPkt, encCtx->time_base,
                             outVideoStream->time_base);
        rc = av_interleaved_write_frame(outFmt, encPkt);
        av_packet_unref(encPkt);
        if (rc < 0) {
            return std::string("VideoFilterGraph: failed to write encoded "
                               "video packet: ") + ffmpegErrorString(rc);
        }
    }
}

std::optional<std::string> ensureConvertedFrame(AVFrame* converted,
                                                const AVFrame* source,
                                                AVCodecContext* encCtx)
{
    if (!converted || !source || !encCtx) {
        return std::string("VideoFilterGraph: invalid conversion frame state");
    }

    const bool needsAllocation =
        !converted->buf[0]
        || converted->format != encCtx->pix_fmt
        || converted->width != encCtx->width
        || converted->height != encCtx->height;

    if (needsAllocation) {
        av_frame_unref(converted);
        converted->format = encCtx->pix_fmt;
        converted->width = encCtx->width;
        converted->height = encCtx->height;
        converted->sample_aspect_ratio = source->sample_aspect_ratio;

        const int rc = av_frame_get_buffer(converted, 32);
        if (rc < 0) {
            return std::string("VideoFilterGraph: failed to allocate converted "
                               "video frame: ") + ffmpegErrorString(rc);
        }
    }

    const int writableRc = av_frame_make_writable(converted);
    if (writableRc < 0) {
        return std::string("VideoFilterGraph: converted video frame is not "
                           "writable: ") + ffmpegErrorString(writableRc);
    }

    return std::nullopt;
}

std::optional<std::string> sendFrameToEncoder(AVFrame* filtered,
                                              AVCodecContext* encCtx,
                                              AVStream* outVideoStream,
                                              AVFormatContext* outFmt,
                                              SwsGuard& sws,
                                              AVFrame* converted,
                                              AVPacket* encPkt,
                                              int64_t& nextGeneratedPts)
{
    AVFrame* frameToSend = filtered;
    if (filtered->format != encCtx->pix_fmt
        || filtered->width != encCtx->width
        || filtered->height != encCtx->height) {
        auto allocationError = ensureConvertedFrame(converted, filtered, encCtx);
        if (allocationError) return allocationError;

        sws.ctx = sws_getCachedContext(
            sws.ctx,
            filtered->width,
            filtered->height,
            static_cast<AVPixelFormat>(filtered->format),
            encCtx->width,
            encCtx->height,
            encCtx->pix_fmt,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!sws.ctx) {
            return std::string("VideoFilterGraph: failed to create swscale "
                               "context for ") +
                   (av_get_pix_fmt_name(static_cast<AVPixelFormat>(filtered->format))
                        ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(filtered->format))
                        : "?") +
                   " -> " +
                   (av_get_pix_fmt_name(encCtx->pix_fmt)
                        ? av_get_pix_fmt_name(encCtx->pix_fmt)
                        : "?");
        }

        const int scaledHeight = sws_scale(
            sws.ctx,
            filtered->data,
            filtered->linesize,
            0,
            filtered->height,
            converted->data,
            converted->linesize);
        if (scaledHeight <= 0) {
            return std::string("VideoFilterGraph: swscale failed");
        }

        converted->pts = filtered->pts;
        converted->duration = filtered->duration;
        converted->sample_aspect_ratio = filtered->sample_aspect_ratio;
        converted->color_range = filtered->color_range;
        converted->color_primaries = filtered->color_primaries;
        converted->color_trc = filtered->color_trc;
        converted->colorspace = filtered->colorspace;
        converted->chroma_location = filtered->chroma_location;
        converted->format = encCtx->pix_fmt;
        converted->width = encCtx->width;
        converted->height = encCtx->height;
        frameToSend = converted;
    }

    if (frameToSend->pts == AV_NOPTS_VALUE) {
        frameToSend->pts = nextGeneratedPts;
    }
    nextGeneratedPts = frameToSend->pts + 1;

    int rc = avcodec_send_frame(encCtx, frameToSend);
    if (rc == AVERROR(EAGAIN)) {
        auto drainError = drainEncoder(encCtx, outVideoStream, outFmt, encPkt);
        if (drainError) return drainError;
        rc = avcodec_send_frame(encCtx, frameToSend);
    }
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to send video frame to "
                           "encoder: ") + ffmpegErrorString(rc);
    }

    return drainEncoder(encCtx, outVideoStream, outFmt, encPkt);
}

std::optional<std::string> drainFilter(AVFilterContext* sinkCtx,
                                       AVCodecContext* encCtx,
                                       AVStream* outVideoStream,
                                       AVFormatContext* outFmt,
                                       AVFrame* filteredFrame,
                                       SwsGuard& sws,
                                       AVFrame* convertedFrame,
                                       AVPacket* encPkt,
                                       int64_t& nextGeneratedPts)
{
    while (true) {
        const int rc = av_buffersink_get_frame(sinkCtx, filteredFrame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return std::nullopt;
        }
        if (rc < 0) {
            av_frame_unref(filteredFrame);
            return std::string("VideoFilterGraph: failed to receive filtered "
                               "video frame: ") + ffmpegErrorString(rc);
        }

        auto encodeError = sendFrameToEncoder(
            filteredFrame,
            encCtx,
            outVideoStream,
            outFmt,
            sws,
            convertedFrame,
            encPkt,
            nextGeneratedPts);
        av_frame_unref(filteredFrame);
        if (encodeError) return encodeError;
    }
}

int clampProgress(int value)
{
    return std::max(0, std::min(100, value));
}

} // namespace

VideoFilterGraph::VideoFilterGraph() = default;

VideoFilterGraph::~VideoFilterGraph() = default;

std::optional<std::string> VideoFilterGraph::run(
    const VideoFilterRequest& req,
    const std::function<void(int)>& progressCallback,
    const std::function<bool()>& cancelCheck)
{
    if (req.inputPath.empty()) {
        return std::string("VideoFilterGraph: input path is empty");
    }
    if (req.outputPath.empty()) {
        return std::string("VideoFilterGraph: output path is empty");
    }
    if (req.filterDescription.empty()) {
        return std::string("VideoFilterGraph: filter description is empty");
    }

    const std::string firstFilter = firstFilterName(req.filterDescription);
    if (!firstFilter.empty() && !avfilter_get_by_name(firstFilter.c_str())) {
        return std::string("VideoFilterGraph: filter '") + firstFilter
               + "' is not available in this libavfilter build";
    }

    InputFormatGuard inFmt;
    int rc = avformat_open_input(&inFmt.ctx, req.inputPath.c_str(),
                                 nullptr, nullptr);
    if (rc < 0) {
        return std::string("VideoFilterGraph: cannot open input '")
               + req.inputPath + "': " + ffmpegErrorString(rc);
    }

    rc = avformat_find_stream_info(inFmt.ctx, nullptr);
    if (rc < 0) {
        return std::string("VideoFilterGraph: cannot read stream info for '")
               + req.inputPath + "': " + ffmpegErrorString(rc);
    }

    int videoStreamIndex = -1;
    for (unsigned i = 0; i < inFmt.ctx->nb_streams; ++i) {
        AVStream* stream = inFmt.ctx->streams[i];
        if (stream && stream->codecpar
            && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex < 0) {
        return std::string("VideoFilterGraph: input has no video stream");
    }

    AVStream* inVideoStream = inFmt.ctx->streams[videoStreamIndex];
    const AVCodec* decoder =
        avcodec_find_decoder(inVideoStream->codecpar->codec_id);
    if (!decoder) {
        return std::string("VideoFilterGraph: no decoder for input video "
                           "codec '") + codecNameFor(inVideoStream->codecpar)
               + "'";
    }

    CodecContextGuard videoDec;
    videoDec.ctx = avcodec_alloc_context3(decoder);
    if (!videoDec.ctx) {
        return std::string("VideoFilterGraph: failed to allocate video "
                           "decoder context");
    }

    rc = avcodec_parameters_to_context(videoDec.ctx, inVideoStream->codecpar);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to copy video decoder "
                           "parameters: ") + ffmpegErrorString(rc);
    }

    rc = avcodec_open2(videoDec.ctx, decoder, nullptr);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to open video decoder: ")
               + ffmpegErrorString(rc);
    }

    FilterGraphGuard filterGraph;
    filterGraph.graph = avfilter_graph_alloc();
    if (!filterGraph.graph) {
        return std::string("VideoFilterGraph: failed to allocate filter graph");
    }

    const AVFilter* bufferSrc = avfilter_get_by_name("buffer");
    const AVFilter* bufferSink = avfilter_get_by_name("buffersink");
    if (!bufferSrc || !bufferSink) {
        return std::string("VideoFilterGraph: required buffer/buffersink "
                           "filters are not available");
    }

    AVFilterContext* bufferSrcCtx = nullptr;
    AVFilterContext* bufferSinkCtx = nullptr;

    const AVRational inputTimeBase =
        validRational(inVideoStream->time_base)
            ? inVideoStream->time_base
            : AVRational{1, AV_TIME_BASE};
    const AVRational sar =
        validRational(videoDec.ctx->sample_aspect_ratio)
            ? videoDec.ctx->sample_aspect_ratio
            : AVRational{1, 1};

    char srcArgs[256];
    std::snprintf(srcArgs, sizeof(srcArgs),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
                  "pixel_aspect=%d/%d",
                  videoDec.ctx->width,
                  videoDec.ctx->height,
                  videoDec.ctx->pix_fmt,
                  inputTimeBase.num,
                  inputTimeBase.den,
                  sar.num,
                  sar.den);

    rc = avfilter_graph_create_filter(&bufferSrcCtx, bufferSrc, "in",
                                      srcArgs, nullptr, filterGraph.graph);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to create buffer source: ")
               + ffmpegErrorString(rc);
    }

    rc = avfilter_graph_create_filter(&bufferSinkCtx, bufferSink, "out",
                                      nullptr, nullptr, filterGraph.graph);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to create buffer sink: ")
               + ffmpegErrorString(rc);
    }

    InOutGuard outputs(avfilter_inout_alloc());
    InOutGuard inputs(avfilter_inout_alloc());
    if (!outputs.io || !inputs.io) {
        return std::string("VideoFilterGraph: failed to allocate filter graph "
                           "endpoints");
    }

    outputs.io->name = av_strdup("in");
    outputs.io->filter_ctx = bufferSrcCtx;
    outputs.io->pad_idx = 0;
    outputs.io->next = nullptr;
    inputs.io->name = av_strdup("out");
    inputs.io->filter_ctx = bufferSinkCtx;
    inputs.io->pad_idx = 0;
    inputs.io->next = nullptr;
    if (!outputs.io->name || !inputs.io->name) {
        return std::string("VideoFilterGraph: failed to allocate filter graph "
                           "endpoint names");
    }

    AVFilterInOut* rawInputs = inputs.io;
    AVFilterInOut* rawOutputs = outputs.io;
    rc = avfilter_graph_parse_ptr(filterGraph.graph,
                                  req.filterDescription.c_str(),
                                  &rawInputs,
                                  &rawOutputs,
                                  nullptr);
    inputs.io = rawInputs;
    outputs.io = rawOutputs;
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to parse filter "
                           "description '") + req.filterDescription + "': "
               + ffmpegErrorString(rc);
    }

    rc = avfilter_graph_config(filterGraph.graph, nullptr);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to configure filter "
                           "graph for '") + req.filterDescription + "': "
               + ffmpegErrorString(rc);
    }

    const AVFilterLink* sinkLink =
        (bufferSinkCtx && bufferSinkCtx->nb_inputs > 0)
            ? bufferSinkCtx->inputs[0]
            : nullptr;
    const int outputWidth =
        sinkLink && sinkLink->w > 0 ? sinkLink->w : videoDec.ctx->width;
    const int outputHeight =
        sinkLink && sinkLink->h > 0 ? sinkLink->h : videoDec.ctx->height;
    const AVPixelFormat filteredPixFmt =
        sinkLink ? static_cast<AVPixelFormat>(sinkLink->format)
                 : videoDec.ctx->pix_fmt;
    const AVRational filterTimeBase =
        sinkLink && validRational(sinkLink->time_base)
            ? sinkLink->time_base
            : inputTimeBase;

    OutputFormatGuard outFmt;
    rc = avformat_alloc_output_context2(&outFmt.ctx, nullptr, nullptr,
                                        req.outputPath.c_str());
    if (rc < 0 || !outFmt.ctx) {
        return std::string("VideoFilterGraph: cannot create output context "
                           "for '") + req.outputPath + "': "
               + ffmpegErrorString(rc);
    }

    const AVCodec* encoder =
        resolveEncoder(req, inVideoStream->codecpar->codec_id);
    if (!encoder) {
        return std::string("VideoFilterGraph: no usable video encoder found");
    }

    CodecContextGuard videoEnc;
    videoEnc.ctx = avcodec_alloc_context3(encoder);
    if (!videoEnc.ctx) {
        return std::string("VideoFilterGraph: failed to allocate video "
                           "encoder context");
    }

    videoEnc.ctx->width = outputWidth;
    videoEnc.ctx->height = outputHeight;
    videoEnc.ctx->sample_aspect_ratio =
        sinkLink && validRational(sinkLink->sample_aspect_ratio)
            ? sinkLink->sample_aspect_ratio
            : sar;
    videoEnc.ctx->pix_fmt =
        chooseEncoderPixelFormat(encoder, filteredPixFmt);
    videoEnc.ctx->time_base = filterTimeBase;
    videoEnc.ctx->framerate = bestFrameRate(inFmt.ctx, inVideoStream);
    videoEnc.ctx->bit_rate =
        req.videoBitrateBits > 0
            ? req.videoBitrateBits
            : (inVideoStream->codecpar->bit_rate > 0
                   ? inVideoStream->codecpar->bit_rate
                   : 4000000);
    if (outFmt.ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        videoEnc.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    DictGuard encoderOptions;
    if (encoder->name
        && (std::string(encoder->name) == "libx264"
            || std::string(encoder->name) == "libx265")) {
        av_dict_set(&encoderOptions.dict, "preset", "medium", 0);
    }

    rc = avcodec_open2(videoEnc.ctx, encoder, &encoderOptions.dict);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to open video encoder '")
               + (encoder->name ? encoder->name : "?") + "': "
               + ffmpegErrorString(rc);
    }

    AVStream* outVideoStream = avformat_new_stream(outFmt.ctx, nullptr);
    if (!outVideoStream) {
        return std::string("VideoFilterGraph: failed to create output video "
                           "stream");
    }

    rc = avcodec_parameters_from_context(outVideoStream->codecpar,
                                         videoEnc.ctx);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to copy video encoder "
                           "parameters: ") + ffmpegErrorString(rc);
    }
    outVideoStream->time_base = videoEnc.ctx->time_base;

    std::vector<int> streamMapping(inFmt.ctx->nb_streams, -1);
    streamMapping[videoStreamIndex] = outVideoStream->index;
    if (req.copyAudio) {
        for (unsigned i = 0; i < inFmt.ctx->nb_streams; ++i) {
            if (static_cast<int>(i) == videoStreamIndex) {
                continue;
            }
            AVStream* inStream = inFmt.ctx->streams[i];
            if (!inStream || !inStream->codecpar) {
                continue;
            }
            AVStream* outStream = avformat_new_stream(outFmt.ctx, nullptr);
            if (!outStream) {
                return std::string("VideoFilterGraph: failed to create "
                                   "stream-copy output stream");
            }
            rc = avcodec_parameters_copy(outStream->codecpar,
                                         inStream->codecpar);
            if (rc < 0) {
                return std::string("VideoFilterGraph: failed to copy "
                                   "parameters for stream ")
                       + std::to_string(i) + ": " + ffmpegErrorString(rc);
            }
            outStream->codecpar->codec_tag = 0;
            outStream->time_base = inStream->time_base;
            streamMapping[i] = outStream->index;
        }
    }

    if (!(outFmt.ctx->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&outFmt.ctx->pb, req.outputPath.c_str(),
                       AVIO_FLAG_WRITE);
        if (rc < 0) {
            return std::string("VideoFilterGraph: cannot open output '")
                   + req.outputPath + "': " + ffmpegErrorString(rc);
        }
    }

    rc = avformat_write_header(outFmt.ctx, nullptr);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to write output header: ")
               + ffmpegErrorString(rc);
    }

    PacketGuard readPacket;
    PacketGuard encPacket;
    FrameGuard decodedFrame;
    FrameGuard filteredFrame;
    FrameGuard convertedFrame;
    if (!readPacket.pkt || !encPacket.pkt || !decodedFrame.frame
        || !filteredFrame.frame || !convertedFrame.frame) {
        return std::string("VideoFilterGraph: failed to allocate packet/frame "
                           "buffers");
    }

    SwsGuard sws;
    const int64_t totalDurationUs = bestDurationUs(inFmt.ctx, inVideoStream);
    int lastProgress = -1;
    bool cancelled = false;
    int64_t nextGeneratedPts = 0;

    auto reportProgress = [&](int progress) {
        if (!progressCallback) return;
        progress = clampProgress(progress);
        if (progress != lastProgress) {
            lastProgress = progress;
            progressCallback(progress);
        }
    };

    auto drainCurrentFilter = [&]() -> std::optional<std::string> {
        return drainFilter(bufferSinkCtx,
                           videoEnc.ctx,
                           outVideoStream,
                           outFmt.ctx,
                           filteredFrame.frame,
                           sws,
                           convertedFrame.frame,
                           encPacket.pkt,
                           nextGeneratedPts);
    };

    reportProgress(0);

    while (true) {
        if (cancelCheck && cancelCheck()) {
            cancelled = true;
            break;
        }

        rc = av_read_frame(inFmt.ctx, readPacket.pkt);
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            return std::string("VideoFilterGraph: failed while reading input: ")
                   + ffmpegErrorString(rc);
        }

        const int packetStreamIndex = readPacket.pkt->stream_index;
        if (packetStreamIndex >= 0
            && static_cast<unsigned>(packetStreamIndex) < inFmt.ctx->nb_streams
            && readPacket.pkt->dts != AV_NOPTS_VALUE) {
            AVStream* progressStream = inFmt.ctx->streams[packetStreamIndex];
            if (progressStream && validRational(progressStream->time_base)) {
                const int64_t packetUs = av_rescale_q(
                    readPacket.pkt->dts,
                    progressStream->time_base,
                    AVRational{1, AV_TIME_BASE});
                const int progress =
                    static_cast<int>((packetUs * 100) / totalDurationUs);
                reportProgress(std::min(progress, 99));
            }
        }

        if (packetStreamIndex == videoStreamIndex) {
            rc = avcodec_send_packet(videoDec.ctx, readPacket.pkt);
            if (rc < 0) {
                av_packet_unref(readPacket.pkt);
                return std::string("VideoFilterGraph: failed to send video "
                                   "packet to decoder: ") + ffmpegErrorString(rc);
            }

            while (true) {
                rc = avcodec_receive_frame(videoDec.ctx, decodedFrame.frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                    break;
                }
                if (rc < 0) {
                    av_packet_unref(readPacket.pkt);
                    return std::string("VideoFilterGraph: failed to decode "
                                       "video frame: ") + ffmpegErrorString(rc);
                }

                rc = av_buffersrc_add_frame_flags(
                    bufferSrcCtx,
                    decodedFrame.frame,
                    AV_BUFFERSRC_FLAG_KEEP_REF);
                av_frame_unref(decodedFrame.frame);
                if (rc < 0) {
                    av_packet_unref(readPacket.pkt);
                    return std::string("VideoFilterGraph: failed to send frame "
                                       "to filter graph: ") + ffmpegErrorString(rc);
                }

                auto drainError = drainCurrentFilter();
                if (drainError) {
                    av_packet_unref(readPacket.pkt);
                    return drainError;
                }
            }
        } else if (packetStreamIndex >= 0
                   && static_cast<size_t>(packetStreamIndex) < streamMapping.size()
                   && streamMapping[packetStreamIndex] >= 0) {
            AVStream* inStream = inFmt.ctx->streams[packetStreamIndex];
            AVStream* outStream =
                outFmt.ctx->streams[streamMapping[packetStreamIndex]];
            readPacket.pkt->stream_index = outStream->index;
            readPacket.pkt->pos = -1;
            av_packet_rescale_ts(readPacket.pkt,
                                 inStream->time_base,
                                 outStream->time_base);
            rc = av_interleaved_write_frame(outFmt.ctx, readPacket.pkt);
            if (rc < 0) {
                av_packet_unref(readPacket.pkt);
                return std::string("VideoFilterGraph: failed to stream-copy "
                                   "packet: ") + ffmpegErrorString(rc);
            }
        }

        av_packet_unref(readPacket.pkt);
    }

    if (!cancelled) {
        rc = avcodec_send_packet(videoDec.ctx, nullptr);
        if (rc < 0 && rc != AVERROR_EOF) {
            return std::string("VideoFilterGraph: failed to flush video "
                               "decoder: ") + ffmpegErrorString(rc);
        }

        while (true) {
            rc = avcodec_receive_frame(videoDec.ctx, decodedFrame.frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                break;
            }
            if (rc < 0) {
                return std::string("VideoFilterGraph: failed to receive "
                                   "flushed video frame: ") + ffmpegErrorString(rc);
            }

            rc = av_buffersrc_add_frame_flags(
                bufferSrcCtx,
                decodedFrame.frame,
                AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_unref(decodedFrame.frame);
            if (rc < 0) {
                return std::string("VideoFilterGraph: failed to send flushed "
                                   "frame to filter graph: ") + ffmpegErrorString(rc);
            }

            auto drainError = drainCurrentFilter();
            if (drainError) return drainError;
        }

        rc = av_buffersrc_add_frame_flags(bufferSrcCtx, nullptr, 0);
        if (rc < 0) {
            return std::string("VideoFilterGraph: failed to flush filter "
                               "graph: ") + ffmpegErrorString(rc);
        }

        auto drainError = drainCurrentFilter();
        if (drainError) return drainError;
    }

    rc = avcodec_send_frame(videoEnc.ctx, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) {
        return std::string("VideoFilterGraph: failed to flush video encoder: ")
               + ffmpegErrorString(rc);
    }
    auto encoderDrainError =
        drainEncoder(videoEnc.ctx, outVideoStream, outFmt.ctx, encPacket.pkt);
    if (encoderDrainError) return encoderDrainError;

    rc = av_write_trailer(outFmt.ctx);
    if (rc < 0) {
        return std::string("VideoFilterGraph: failed to write output trailer: ")
               + ffmpegErrorString(rc);
    }

    if (cancelled) {
        reportProgress(0);
        return std::string("VideoFilterGraph: cancelled");
    }

    reportProgress(100);
    return std::nullopt;
}

} // namespace libavcore
