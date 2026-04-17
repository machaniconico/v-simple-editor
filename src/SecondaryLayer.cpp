#include "SecondaryLayer.h"
#include <QDebug>

extern "C" {
#include <libavutil/imgutils.h>
}

SecondaryLayer::SecondaryLayer() = default;

SecondaryLayer::~SecondaryLayer()
{
    close();
}

int SecondaryLayer::width() const
{
    return (m_codecCtx) ? m_codecCtx->width : 0;
}

int SecondaryLayer::height() const
{
    return (m_codecCtx) ? m_codecCtx->height : 0;
}

double SecondaryLayer::aspectRatio() const
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) return 0.0;
    return static_cast<double>(w) / static_cast<double>(h);
}

bool SecondaryLayer::open(const QString &filePath,
                          AVBufferRef *sharedHwDeviceCtx)
{
    close();
    m_filePath = filePath;

    const QByteArray pathUtf8 = filePath.toUtf8();
    if (avformat_open_input(&m_formatCtx, pathUtf8.constData(), nullptr, nullptr) != 0) {
        qWarning() << "SecondaryLayer::open avformat_open_input failed" << filePath;
        close();
        return false;
    }
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qWarning() << "SecondaryLayer::open avformat_find_stream_info failed";
        close();
        return false;
    }

    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; ++i) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIndex < 0) {
        qWarning() << "SecondaryLayer::open no video stream" << filePath;
        close();
        return false;
    }

    if (!openCodec(sharedHwDeviceCtx)) {
        close();
        return false;
    }

    m_frame   = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet  = av_packet_alloc();
    if (!m_frame || !m_swFrame || !m_packet) {
        qWarning() << "SecondaryLayer::open frame/packet alloc failed";
        close();
        return false;
    }

    qInfo() << "SecondaryLayer::open OK" << filePath
            << "stream#" << m_videoStreamIndex
            << "size" << width() << "x" << height()
            << "hwPixFmt=" << m_hwPixFmt;
    return true;
}

enum AVPixelFormat SecondaryLayer::getHwFormatCallback(AVCodecContext *ctx,
                                                       const enum AVPixelFormat *pixFmts)
{
    auto *self = static_cast<SecondaryLayer*>(ctx->opaque);
    if (!self || self->m_hwPixFmt == AV_PIX_FMT_NONE)
        return pixFmts[0];
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->m_hwPixFmt)
            return *p;
    }
    qWarning() << "SecondaryLayer: HW pixel format not offered by decoder, falling back to SW";
    self->m_hwPixFmt = AV_PIX_FMT_NONE;
    return pixFmts[0];
}

bool SecondaryLayer::openCodec(AVBufferRef *sharedHwDeviceCtx)
{
    auto *codecpar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        qWarning() << "SecondaryLayer::openCodec unsupported codec id" << codecpar->codec_id;
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;
    if (avcodec_parameters_to_context(m_codecCtx, codecpar) < 0) {
        return false;
    }

    // Stage 2.7 — share VideoPlayer's D3D11VA context when one is available.
    // We probe the codec's supported HW configs to find a matching pixel
    // format; if that exists we bind hw_device_ctx and register the same
    // get_format callback pattern the primary decoder uses. A failure here
    // silently falls back to SW decode so AV1 / VP9 on older drivers still
    // work.
    if (sharedHwDeviceCtx) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                m_hwPixFmt = cfg->pix_fmt;
                break;
            }
        }
        if (m_hwPixFmt != AV_PIX_FMT_NONE) {
            m_hwDeviceCtx = av_buffer_ref(sharedHwDeviceCtx);
            m_codecCtx->hw_device_ctx = av_buffer_ref(sharedHwDeviceCtx);
            m_codecCtx->opaque     = this;
            m_codecCtx->get_format = &SecondaryLayer::getHwFormatCallback;
        }
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        qWarning() << "SecondaryLayer::openCodec avcodec_open2 failed";
        return false;
    }
    return true;
}

void SecondaryLayer::close()
{
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_frame)   av_frame_free(&m_frame);
    if (m_swFrame) av_frame_free(&m_swFrame);
    if (m_packet)  av_packet_free(&m_packet);
    if (m_codecCtx)  avcodec_free_context(&m_codecCtx);
    if (m_formatCtx) avformat_close_input(&m_formatCtx);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_hwPixFmt         = AV_PIX_FMT_NONE;
    m_videoStreamIndex = -1;
    m_swsWidth    = 0;
    m_swsHeight   = 0;
    m_swsSrcFormat = AV_PIX_FMT_NONE;
    m_filePath.clear();
    m_lastImage = QImage();
    m_lastPtsUs = 0;
}

int64_t SecondaryLayer::ptsToUs(int64_t pts) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0 || pts == AV_NOPTS_VALUE) return 0;
    const AVStream *st = m_formatCtx->streams[m_videoStreamIndex];
    const AVRational tb = st->time_base;
    if (tb.den <= 0) return 0;
    return av_rescale_q(pts, tb, AVRational{1, AV_TIME_BASE});
}

int64_t SecondaryLayer::usToPts(int64_t us) const
{
    if (!m_formatCtx || m_videoStreamIndex < 0) return 0;
    const AVStream *st = m_formatCtx->streams[m_videoStreamIndex];
    const AVRational tb = st->time_base;
    if (tb.num <= 0) return 0;
    return av_rescale_q(us, AVRational{1, AV_TIME_BASE}, tb);
}

bool SecondaryLayer::seekToFileUs(int64_t fileLocalUs, bool precise)
{
    if (!isOpen()) return false;
    const int64_t seekPts = usToPts(qMax<int64_t>(0, fileLocalUs));
    const int seekFlags = AVSEEK_FLAG_BACKWARD;
    if (av_seek_frame(m_formatCtx, m_videoStreamIndex, seekPts, seekFlags) < 0) {
        qWarning() << "SecondaryLayer::seekToFileUs av_seek_frame failed";
        return false;
    }
    avcodec_flush_buffers(m_codecCtx);

    if (!precise) {
        // Still decode one frame so lastImage() reflects the seek target.
        (void)decodeNextFrame();
        return true;
    }

    // Precise seek: decode until we land at or past the requested time.
    const int64_t targetPts = seekPts;
    while (true) {
        int64_t ptsUs = 0;
        QImage img = decodeNextFrame(&ptsUs);
        if (img.isNull()) return false;
        if (m_frame->pts == AV_NOPTS_VALUE) break;
        if (m_frame->pts >= targetPts) break;
    }
    return true;
}

QImage SecondaryLayer::decodeNextFrame(int64_t *ptsUsOut)
{
    if (!isOpen() || !m_frame || !m_packet) return QImage();

    auto receive = [this, ptsUsOut]() -> QImage {
        const int rc = avcodec_receive_frame(m_codecCtx, m_frame);
        if (rc == 0) {
            const int64_t beb = (m_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? m_frame->best_effort_timestamp : m_frame->pts;
            const int64_t ptsUs = (beb != AV_NOPTS_VALUE) ? ptsToUs(beb) : m_lastPtsUs;

            // Stage 2.7 — HW decode surfaces (D3D11VA / NV12 on GPU memory)
            // must be transferred to SW before sws_scale can touch them.
            AVFrame *displayable = m_frame;
            if (m_hwPixFmt != AV_PIX_FMT_NONE
                && m_frame->format == m_hwPixFmt) {
                av_frame_unref(m_swFrame);
                const int xfer = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
                if (xfer < 0) {
                    qWarning() << "SecondaryLayer: av_hwframe_transfer_data failed" << xfer;
                    return QImage();
                }
                m_swFrame->pts                 = m_frame->pts;
                m_swFrame->best_effort_timestamp = m_frame->best_effort_timestamp;
                displayable = m_swFrame;
            }

            QImage img = frameToRgba(displayable);
            if (!img.isNull()) {
                m_lastImage = img;
                m_lastPtsUs = ptsUs;
                if (ptsUsOut) *ptsUsOut = ptsUs;
            }
            return img;
        }
        return QImage();
    };

    QImage out = receive();
    if (!out.isNull()) return out;

    while (av_read_frame(m_formatCtx, m_packet) >= 0) {
        if (m_packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }
        const int sendRc = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);
        if (sendRc < 0) continue;
        out = receive();
        if (!out.isNull()) return out;
    }

    // Flush
    if (avcodec_send_packet(m_codecCtx, nullptr) >= 0) {
        out = receive();
        if (!out.isNull()) return out;
    }
    return QImage();
}

QImage SecondaryLayer::frameToRgba(AVFrame *frame)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) return QImage();

    const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
    if (!m_swsCtx
        || m_swsWidth != frame->width
        || m_swsHeight != frame->height
        || m_swsSrcFormat != srcFmt) {
        if (m_swsCtx) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }
        m_swsCtx = sws_getContext(frame->width, frame->height, srcFmt,
                                  frame->width, frame->height, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_swsCtx) {
            qWarning() << "SecondaryLayer: sws_getContext failed";
            return QImage();
        }
        m_swsWidth    = frame->width;
        m_swsHeight   = frame->height;
        m_swsSrcFormat = srcFmt;
    }

    QImage img(frame->width, frame->height, QImage::Format_RGBA8888);
    if (img.isNull()) return QImage();

    uint8_t *dstData[4]  = { img.bits(), nullptr, nullptr, nullptr };
    int      dstLine[4]  = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };
    if (sws_scale(m_swsCtx, frame->data, frame->linesize, 0, frame->height,
                  dstData, dstLine) <= 0) {
        return QImage();
    }
    return img;
}
