#include "RotoApply.h"

#include "Rotoscope.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace roto {
namespace {
std::atomic<quint64> gSourceFrameIndexForCalls{0};
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
