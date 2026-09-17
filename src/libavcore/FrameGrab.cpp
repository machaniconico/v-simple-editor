#include "FrameGrab.h"
#include <QFileInfo>
#include <QStringList>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

bool libavcore::isStillImage(const QString &path)
{
    static const QStringList extensions = {"png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp", "exr", "tga", "ppm", "pgm"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

namespace {
bool openVideoDecoder(const QString &filePath,
                      AVFormatContext **fmtCtx,
                      AVCodecContext **decCtx,
                      int *streamIndex)
{
    *fmtCtx = nullptr;
    *decCtx = nullptr;
    *streamIndex = -1;

    if (avformat_open_input(fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(*fmtCtx, nullptr) < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    for (unsigned i = 0; i < (*fmtCtx)->nb_streams; ++i) {
        if ((*fmtCtx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (*streamIndex < 0) {
        avformat_close_input(fmtCtx);
        return false;
    }

    const AVCodecParameters *codecpar = (*fmtCtx)->streams[*streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        avformat_close_input(fmtCtx);
        return false;
    }

    *decCtx = avcodec_alloc_context3(codec);
    if (!*decCtx) {
        avformat_close_input(fmtCtx);
        return false;
    }
    if (avcodec_parameters_to_context(*decCtx, codecpar) < 0
        || avcodec_open2(*decCtx, codec, nullptr) < 0) {
        avcodec_free_context(decCtx);
        avformat_close_input(fmtCtx);
        return false;
    }
    return true;
}

bool scaleFrameToQImagePadded(SwsContext *ctx,
                              const AVFrame *frame,
                              AVPixelFormat dstPixFmt,
                              QImage &image)
{
    if (!ctx || !frame || image.isNull())
        return false;

    const int rowBytes = av_image_get_linesize(dstPixFmt, image.width(), 0);
    if (rowBytes <= 0 || rowBytes > image.bytesPerLine())
        return false;

    uint8_t *tmpData[4] = { nullptr, nullptr, nullptr, nullptr };
    int tmpStride[4] = { 0, 0, 0, 0 };
    if (av_image_alloc(tmpData, tmpStride, image.width(), image.height(),
                       dstPixFmt, 64) < 0)
        return false;

    sws_scale(ctx, frame->data, frame->linesize, 0, frame->height,
              tmpData, tmpStride);
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(image.scanLine(y), tmpData[0] + y * tmpStride[0],
                    static_cast<std::size_t>(rowBytes));
    }
    av_freep(&tmpData[0]);
    return true;
}

QImage avFrameToQImage(const AVFrame *frame, AVCodecContext *decCtx)
{
    if (!frame || !decCtx)
        return {};

    SwsContext *toRgbCtx = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                          frame->width, frame->height, AV_PIX_FMT_RGBA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!toRgbCtx)
        return {};

    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);
    if (!scaleFrameToQImagePadded(toRgbCtx, frame, AV_PIX_FMT_RGBA, image)) {
        sws_freeContext(toRgbCtx);
        return {};
    }
    sws_freeContext(toRgbCtx);
    return image;
}

}

QImage libavcore::grabFrameAt(const QString &filePath, double sourceTimeSeconds, QSize maxSize)
{
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *decCtx = nullptr;
    int streamIndex = -1;
    if (!openVideoDecoder(filePath, &fmtCtx, &decCtx, &streamIndex))
        return {};

    AVStream *stream = fmtCtx->streams[streamIndex];
    const bool still = isStillImage(filePath);
    const double targetSeconds = still || !std::isfinite(sourceTimeSeconds)
        ? 0.0 : qBound(0.0, sourceTimeSeconds, 1.0e10);
    const int64_t seekTarget = av_rescale_q(
        static_cast<int64_t>(targetSeconds * AV_TIME_BASE),
        AVRational{1, AV_TIME_BASE},
        stream->time_base);
    if (!still) {
        av_seek_frame(fmtCtx, streamIndex, seekTarget, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decCtx);
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    QImage result;

    if (!packet || !frame)
        goto decode_done;
    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }
        if (avcodec_send_packet(decCtx, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);

        while (avcodec_receive_frame(decCtx, frame) == 0) {
            const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame->best_effort_timestamp
                : frame->pts;
            const double frameSeconds = (pts != AV_NOPTS_VALUE)
                ? pts * av_q2d(stream->time_base)
                : targetSeconds;
            result = avFrameToQImage(frame, decCtx);
            if (result.isNull() || (!still && frameSeconds + 1.0 / 120.0 < targetSeconds))
                continue;
            goto decode_done;
        }
    }

decode_done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    if (!result.isNull() && maxSize.isValid() && !maxSize.isEmpty()
        && (result.width() > maxSize.width() || result.height() > maxSize.height()))
        result = result.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
    return result;
}
