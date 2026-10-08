#include "RotoApply.h"

#include "Rotoscope.h"
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <atomic>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
}

namespace roto {
namespace {
std::atomic<quint64> gSourceFrameIndexForCalls{0};
std::atomic<quint64> gRenderApplyCalls{0};
std::atomic<int> gLastRenderSourceFrameIndex{-1};
std::atomic<quint64> gProbeVideoStreamFpsOpens{0};
} // namespace

QImage applyEntryToFrame(const RotoClipEntry &entry, const QImage &frame,
                         int sourceFrameIndex)
{
    if (frame.isNull())
        return frame;

    QImage mask;
    if (!entry.keyframes.isEmpty()) {
        Rotoscope rotoscope;
        for (const auto &keyframe : entry.keyframes)
            rotoscope.addKeyframe(keyframe.frameNumber, keyframe.path);
        mask = rotoscope.renderMask(sourceFrameIndex, frame.size());
    } else if (!entry.path.points.isEmpty()) {
        Rotoscope rotoscope;
        rotoscope.addKeyframe(sourceFrameIndex, entry.path);
        mask = rotoscope.renderMask(sourceFrameIndex, frame.size());
    }
    if (!entry.brushMask.isNull())
        mask = combineMasksMax(mask, entry.brushMask);
    if (mask.isNull())
        return frame;
    return Rotoscope::applyToFrame(frame, mask);
}

QImage applyEntryForRender(const RotoClipEntry &entry, const QImage &frame,
                           int sourceFrameIndex)
{
    gRenderApplyCalls.fetch_add(1, std::memory_order_relaxed);
    gLastRenderSourceFrameIndex.store(sourceFrameIndex, std::memory_order_relaxed);
    const QImage applied = applyEntryToFrame(entry, frame, sourceFrameIndex);
    return applied.format() == frame.format()
        ? applied : applied.convertToFormat(frame.format());
}

quint64 renderApplyCallCount()
{
    return gRenderApplyCalls.load(std::memory_order_relaxed);
}

int lastRenderSourceFrameIndex()
{
    return gLastRenderSourceFrameIndex.load(std::memory_order_relaxed);
}

quint64 probeVideoStreamFpsOpenCount()
{
    return gProbeVideoStreamFpsOpens.load(std::memory_order_relaxed);
}

double probeVideoStreamFps(const QString &filePath)
{
    double fps = 0.0;
    if (filePath.isEmpty() || filePath.startsWith(QStringLiteral("veditor://")))
        return fps;

    // Process-wide cache (the renderer probes every roto frame, possibly on
    // export worker threads). A missing file or a failed open is not cached,
    // so relinked media is probed again.
    static QMutex cacheMutex;
    static QHash<QString, double> cache;
    const QFileInfo info(filePath);
    QString cacheKey;
    if (info.exists()) {
        cacheKey = info.absoluteFilePath() + QLatin1Char('|') + QString::number(info.size())
            + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
        QMutexLocker locker(&cacheMutex);
        const auto it = cache.constFind(cacheKey);
        if (it != cache.constEnd())
            return it.value();
    }

    gProbeVideoStreamFpsOpens.fetch_add(1, std::memory_order_relaxed);
    AVFormatContext *format = nullptr;
    if (avformat_open_input(&format, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return fps;
    if (avformat_find_stream_info(format, nullptr) >= 0) {
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            AVStream *stream = format->streams[i];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                continue;
            const AVRational rate = av_guess_frame_rate(format, stream, nullptr);
            if (rate.num > 0 && rate.den > 0)
                fps = av_q2d(rate);
            break;
        }
    }
    avformat_close_input(&format);
    if (!cacheKey.isEmpty()) {
        QMutexLocker locker(&cacheMutex);
        cache.insert(cacheKey, fps);
    }
    return fps;
}

QImage combineMasksMax(const QImage &a, const QImage &b)
{
    if (a.isNull())
        return b.convertToFormat(QImage::Format_Grayscale8);
    if (b.isNull())
        return a.convertToFormat(QImage::Format_Grayscale8);

    QImage base = a.convertToFormat(QImage::Format_Grayscale8);
    QImage overlay = b.convertToFormat(QImage::Format_Grayscale8);
    if (overlay.size() != base.size()) {
        overlay = overlay.scaled(base.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                         .convertToFormat(QImage::Format_Grayscale8);
    }

    for (int y = 0; y < base.height(); ++y) {
        uchar *baseLine = base.scanLine(y);
        const uchar *overlayLine = overlay.constScanLine(y);
        for (int x = 0; x < base.width(); ++x)
            baseLine[x] = std::max(baseLine[x], overlayLine[x]);
    }
    return base;
}

int sourceFrameIndexFor(double sourceSec, double fps)
{
    gSourceFrameIndexForCalls.fetch_add(1, std::memory_order_relaxed);
    return qMax(0, static_cast<int>(std::llround(sourceSec * fps)));
}

double clipEffectiveSourceFps(double probedFps, double fallbackFps)
{
    return (probedFps > 0.0) ? probedFps : qMax(1.0, fallbackFps);
}

quint64 sourceFrameIndexForCallCount()
{
    return gSourceFrameIndexForCalls.load(std::memory_order_relaxed);
}

} // namespace roto
