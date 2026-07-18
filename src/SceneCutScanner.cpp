#include "SceneCutScanner.h"
#include "SceneDetector.h"

#include <QThread>
#include <QImage>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace {

struct AvImageBuffer {
    uint8_t* data[4] = { nullptr, nullptr, nullptr, nullptr };
    int linesize[4] = { 0, 0, 0, 0 };

    ~AvImageBuffer()
    {
        av_freep(&data[0]);
    }

    AvImageBuffer() = default;
    AvImageBuffer(const AvImageBuffer&) = delete;
    AvImageBuffer& operator=(const AvImageBuffer&) = delete;

    bool allocateGray8(int width, int height)
    {
        return av_image_alloc(data, linesize, width, height,
                              AV_PIX_FMT_GRAY8, 64) >= 0;
    }
};

static void copyVisibleGray8Rows(const uint8_t* src,
                                 int srcLinesize,
                                 int width,
                                 int height,
                                 uint8_t* dst)
{
    for (int y = 0; y < height; ++y)
        std::memcpy(dst + y * width, src + y * srcLinesize, width);
}

} // namespace

SceneCutScanner::SceneCutScanner(QObject* parent)
    : QObject(parent)
{
}

SceneCutScanner::~SceneCutScanner()
{
    // If a scan thread is still running, cancel and wait for it.
    if (m_thread && m_thread->isRunning()) {
        m_cancelled = true;
        m_thread->wait();
    }
}

void SceneCutScanner::scanFile(const QString& path, double threshold, int minSceneFrames)
{
    // Cancel any previously running scan.
    if (m_thread && m_thread->isRunning()) {
        m_cancelled = true;
        m_thread->wait();
    }

    m_cancelled = false;
    m_cutFrames.clear();
    m_cutTimestampsUs.clear();
    m_frameRate = 0.0;
    m_totalFrames = 0;

    QThread* thread = QThread::create([this, path, threshold, minSceneFrames]() {
        doScan(path, threshold, minSceneFrames);
    });
    m_thread = thread;
    connect(thread, &QThread::destroyed, this, [this, thread]() {
        if (m_thread == thread)
            m_thread = nullptr;
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    m_thread->start();
}

void SceneCutScanner::cancel()
{
    m_cancelled = true;
}

QVector<int> SceneCutScanner::cutFrames() const
{
    return m_cutFrames;
}

QVector<qint64> SceneCutScanner::cutTimestampsUs() const
{
    return m_cutTimestampsUs;
}

double SceneCutScanner::frameRate() const
{
    return m_frameRate;
}

int SceneCutScanner::totalFrames() const
{
    return m_totalFrames;
}

// ---------------------------------------------------------------------------
// Internal scan loop — runs on m_thread
// ---------------------------------------------------------------------------
void SceneCutScanner::doScan(QString path, double threshold, int minSceneFrames)
{
    m_cutFrames.clear();
    m_cutTimestampsUs.clear();

    // ------------------------------------------------------------------
    // 1. Open input
    // ------------------------------------------------------------------
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        emit finished(false, QStringLiteral("failed to open input: ") + path);
        return;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("failed to find stream info: ") + path);
        return;
    }

    // ------------------------------------------------------------------
    // 2. Find first video stream
    // ------------------------------------------------------------------
    int videoStreamIdx = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIdx < 0) {
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("no video stream found"));
        return;
    }

    AVStream* stream = fmtCtx->streams[videoStreamIdx];

    // ------------------------------------------------------------------
    // 3. Open decoder
    // ------------------------------------------------------------------
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("no decoder found for video stream"));
        return;
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(codec);
    if (!decCtx) {
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("failed to alloc codec context"));
        return;
    }
    int paramsRet = avcodec_parameters_to_context(decCtx, stream->codecpar);
    if (paramsRet < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("failed to copy codec parameters"));
        return;
    }
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("failed to open decoder"));
        return;
    }

    // ------------------------------------------------------------------
    // 4. Gather stream metadata
    // ------------------------------------------------------------------
    m_frameRate = (stream->avg_frame_rate.den > 0)
                      ? av_q2d(stream->avg_frame_rate)
                      : 30.0;

    // nb_frames may be 0 for some containers; fall back to duration-based estimate.
    if (stream->nb_frames > 0) {
        m_totalFrames = static_cast<int>(stream->nb_frames);
    } else if (fmtCtx->duration > 0 && m_frameRate > 0.0) {
        m_totalFrames = static_cast<int>(
            static_cast<double>(fmtCtx->duration) / AV_TIME_BASE * m_frameRate + 0.5);
    } else {
        m_totalFrames = 0;  // unknown
    }

    // ------------------------------------------------------------------
    // 5. Configure SceneDetector
    // ------------------------------------------------------------------
    SceneDetector detector;
    detector.setThreshold(threshold);
    detector.setMinSceneFrames(minSceneFrames);

    // Connect signal so we can accumulate results as they arrive.
    // Note: detector lives on this thread (m_thread), so direct connection is fine.
    connect(&detector, &SceneDetector::sceneCutDetected,
            this, [this](int frameIndex, qint64 timeUs) {
                m_cutFrames.append(frameIndex);
                m_cutTimestampsUs.append(timeUs);
            }, Qt::DirectConnection);

    // ------------------------------------------------------------------
    // 6. Allocate decode + scale buffers (once, reused each frame)
    // ------------------------------------------------------------------
    // Target width: ~256 px; height scaled to preserve aspect ratio.
    const int srcW = decCtx->width;
    const int srcH = decCtx->height;
    const int dstW = (srcW > 0) ? qMin(srcW, 256) : 256;
    const int dstH = (srcW > 0 && srcH > 0)
                         ? qMax(1, static_cast<int>(static_cast<double>(srcH) * dstW / srcW))
                         : 144;

    SwsContext* swsCtx = sws_getContext(
        srcW, srcH, decCtx->pix_fmt,
        dstW, dstH, AV_PIX_FMT_GRAY8,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    // Allocate a persistent output buffer for the scaled frame.
    const int bufSize = av_image_get_buffer_size(AV_PIX_FMT_GRAY8, dstW, dstH, 1);
    QVector<uint8_t> scaleBuf(bufSize);
    AvImageBuffer paddedScaleBuf;
    if (swsCtx && !paddedScaleBuf.allocateGray8(dstW, dstH)) {
        sws_freeContext(swsCtx);
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        emit finished(false, QStringLiteral("failed to allocate scale buffer"));
        return;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();

    // ------------------------------------------------------------------
    // 7. Decode loop
    // ------------------------------------------------------------------
    int frameIndex = 0;
    int processed  = 0;

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            av_packet_unref(packet);
            break;
        }

        if (packet->stream_index != videoStreamIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);

        while (avcodec_receive_frame(decCtx, frame) == 0) {
            if (m_cancelled.load(std::memory_order_relaxed)) {
                av_frame_unref(frame);
                break;
            }

            // Scale to small grayscale image.
            if (swsCtx) {
                sws_scale(swsCtx,
                          frame->data, frame->linesize,
                          0, frame->height,
                          paddedScaleBuf.data, paddedScaleBuf.linesize);
                copyVisibleGray8Rows(paddedScaleBuf.data[0],
                                     paddedScaleBuf.linesize[0],
                                     dstW, dstH,
                                     scaleBuf.data());

                // Wrap into QImage (no copy — the buffer lives for the call duration).
                QImage img(scaleBuf.data(), dstW, dstH,
                           dstW, QImage::Format_Grayscale8);

                detector.processFrame(frameIndex, img, m_frameRate);
            }

            ++frameIndex;
            ++processed;

            // Emit progress (monotonically increasing).
            if (m_totalFrames > 0) {
                const int pct = static_cast<int>(
                    100LL * processed / m_totalFrames);
                emit progressChanged(qMin(pct, 99));  // 100 emitted on finished
            }

            av_frame_unref(frame);
        }

        if (m_cancelled.load(std::memory_order_relaxed))
            break;
    }

    // ------------------------------------------------------------------
    // 8. Flush decoder
    // ------------------------------------------------------------------
    if (!m_cancelled.load(std::memory_order_relaxed)) {
        avcodec_send_packet(decCtx, nullptr);
        while (avcodec_receive_frame(decCtx, frame) == 0) {
            if (m_cancelled.load(std::memory_order_relaxed)) {
                av_frame_unref(frame);
                break;
            }
            if (swsCtx) {
                sws_scale(swsCtx,
                          frame->data, frame->linesize,
                          0, frame->height,
                          paddedScaleBuf.data, paddedScaleBuf.linesize);
                copyVisibleGray8Rows(paddedScaleBuf.data[0],
                                     paddedScaleBuf.linesize[0],
                                     dstW, dstH,
                                     scaleBuf.data());
                QImage img(scaleBuf.data(), dstW, dstH,
                           dstW, QImage::Format_Grayscale8);
                detector.processFrame(frameIndex, img, m_frameRate);
            }
            ++frameIndex;
            ++processed;
            av_frame_unref(frame);
        }
    }

    // ------------------------------------------------------------------
    // 9. Release all resources
    // ------------------------------------------------------------------
    if (swsCtx)
        sws_freeContext(swsCtx);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    // ------------------------------------------------------------------
    // 10. Emit result
    // ------------------------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        m_cutFrames.clear();
        m_cutTimestampsUs.clear();
        emit finished(false, QStringLiteral("cancelled"));
    } else {
        emit progressChanged(100);
        emit finished(true,
            QStringLiteral("Scanned %1 frames, %2 cuts found")
                .arg(processed)
                .arg(m_cutFrames.size()));
    }
}
