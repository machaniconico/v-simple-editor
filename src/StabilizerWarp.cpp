#include "StabilizerWarp.h"

#include "Timeline.h" // ClipInfo

#include <QPointF>

#include <algorithm>
#include <cmath>

namespace stabwarp {

StabSample sampleAt(const QVector<StabilizerKeyframe> &keyframes, qint64 sourceUs)
{
    StabSample sample;
    if (keyframes.isEmpty())
        return sample;

    // Binary search for the first keyframe with timeUs >= sourceUs.
    auto cmp = [](const StabilizerKeyframe &kf, qint64 t) { return kf.timeUs < t; };
    auto it = std::lower_bound(keyframes.constBegin(), keyframes.constEnd(), sourceUs, cmp);
    const int idx = static_cast<int>(it - keyframes.constBegin());

    if (idx <= 0) {
        const auto &kf = keyframes.first();
        sample = {kf.dx, kf.dy, kf.theta, kf.scale};
    } else if (idx >= keyframes.size()) {
        const auto &kf = keyframes.last();
        sample = {kf.dx, kf.dy, kf.theta, kf.scale};
    } else {
        const auto &a = keyframes[idx - 1];
        const auto &b = keyframes[idx];
        const qint64 span = b.timeUs - a.timeUs;
        const double t = (span > 0)
            ? static_cast<double>(sourceUs - a.timeUs) / static_cast<double>(span)
            : 0.0;
        sample.dx    = a.dx    + (b.dx    - a.dx)    * t;
        sample.dy    = a.dy    + (b.dy    - a.dy)    * t;
        sample.theta = a.theta + (b.theta - a.theta) * t;
        sample.scale = a.scale + (b.scale - a.scale) * t;
    }
    return sample;
}

// The keyframe deltas are in SOURCE pixels and the warp runs in normalised
// (0..1) UV space, so they are divided by the frame size (1920x1080 when
// unknown so we never divide by zero).
static double frameWidthOr(int frameW) { return frameW <= 0 ? 1920.0 : double(frameW); }
static double frameHeightOr(int frameH) { return frameH <= 0 ? 1080.0 : double(frameH); }

std::array<float, 9> inverseMatrixUv(const StabSample &sample, int frameW, int frameH)
{
    const double dxUv = sample.dx / frameWidthOr(frameW);
    const double dyUv = sample.dy / frameHeightOr(frameH);

    // Inverse 2D affine in (cx, cy, 1) space:
    //   c' = (1/s) * R(-theta) * c - (dxUv, dyUv)
    // where R(-theta) = [ cosT  sinT; -sinT  cosT ] (note flipped signs vs R(theta))
    const double invS = (std::abs(sample.scale) > 1e-9) ? 1.0 / sample.scale : 1.0;
    const double cT = std::cos(-sample.theta);
    const double sT = std::sin(-sample.theta);

    // Row-major 3x3 matrix [m11 m12 m13; m21 m22 m23; 0 0 1].
    // q.x = invS*(cT*cx + sT*cy) - dxUv
    // q.y = invS*(-sT*cx + cT*cy) - dyUv
    return {static_cast<float>(invS * cT),  static_cast<float>(invS * sT),
            static_cast<float>(-dxUv),
            static_cast<float>(invS * -sT), static_cast<float>(invS * cT),
            static_cast<float>(-dyUv),
            0.0f, 0.0f, 1.0f};
}

bool isIdentity(const StabSample &sample, int frameW, int frameH)
{
    constexpr double kIdEps = 1e-6;
    return std::abs(sample.dx / frameWidthOr(frameW)) <= kIdEps
        && std::abs(sample.dy / frameHeightOr(frameH)) <= kIdEps
        && std::abs(sample.theta) <= kIdEps
        && std::abs(sample.scale - 1.0) <= kIdEps;
}

QTransform inverseTransform(const StabSample &sample, int frameW, int frameH)
{
    return inverseTransform(sample, frameW, frameH, frameW, frameH);
}

QTransform inverseTransform(const StabSample &sample, int srcW, int srcH, int dstW, int dstH)
{
    const double w = frameWidthOr(dstW);
    const double h = frameHeightOr(dstH);
    const std::array<float, 9> m = inverseMatrixUv(sample, srcW, srcH);
    // QTransform maps row vectors: x' = m11*x + m21*y + dx, y' = m12*x + m22*y + dy,
    // and (a * b) applies a first. Pixel centre -> centred UV -> matrix -> pixel
    // centre, so a non-square frame rotates in UV space exactly as the shader does.
    const QTransform toUv(1.0 / w, 0.0, 0.0, 1.0 / h, 0.5 / w - 0.5, 0.5 / h - 0.5);
    const QTransform uvMatrix(m[0], m[3], m[1], m[4], m[2], m[5]);
    const QTransform toPixel(w, 0.0, 0.0, h, 0.5 * w - 0.5, 0.5 * h - 0.5);
    return toUv * uvMatrix * toPixel;
}

template <typename Channel>
static void warpPixels(const QImage &src, QImage &dst, const QTransform &inverse, int maxValue)
{
    const int w = src.width();
    const int h = src.height();
    for (int y = 0; y < h; ++y) {
        Channel *out = reinterpret_cast<Channel *>(dst.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QPointF p = inverse.map(QPointF(x, y));
            // Clamp before the int conversion so a far-out point cannot overflow.
            const double px = qBound(-1.0, p.x(), double(w));
            const double py = qBound(-1.0, p.y(), double(h));
            const double fx = std::floor(px);
            const double fy = std::floor(py);
            const float ax = static_cast<float>(px - fx);
            const float ay = static_cast<float>(py - fy);
            const int x0 = qBound(0, int(fx), w - 1);
            const int x1 = qBound(0, int(fx) + 1, w - 1);
            const int y0 = qBound(0, int(fy), h - 1);
            const int y1 = qBound(0, int(fy) + 1, h - 1);
            const Channel *r0 = reinterpret_cast<const Channel *>(src.constScanLine(y0));
            const Channel *r1 = reinterpret_cast<const Channel *>(src.constScanLine(y1));
            for (int c = 0; c < 4; ++c) {
                const float v00 = r0[x0 * 4 + c];
                const float v10 = r0[x1 * 4 + c];
                const float v01 = r1[x0 * 4 + c];
                const float v11 = r1[x1 * 4 + c];
                const float top = v00 + (v10 - v00) * ax;
                const float bottom = v01 + (v11 - v01) * ax;
                const float v = top + (bottom - top) * ay;
                out[x * 4 + c] = static_cast<Channel>(qBound(0, int(v + 0.5f), maxValue));
            }
        }
    }
}

QImage warpClampToEdge(const QImage &src, const QTransform &inverse)
{
    if (src.isNull())
        return src;
    switch (src.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied: {
        QImage dst = src.copy(); // keeps the format and metadata
        warpPixels<uchar>(src, dst, inverse, 255);
        return dst;
    }
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied: {
        QImage dst = src.copy();
        warpPixels<quint16>(src, dst, inverse, 65535);
        return dst;
    }
    default:
        // Other layouts go through premultiplied ARGB32 and back to their format.
        return warpClampToEdge(src.convertToFormat(QImage::Format_ARGB32_Premultiplied), inverse)
            .convertToFormat(src.format());
    }
}

QImage applyForExport(const QImage &frame, const ClipInfo &clip, int trackIdx,
                      int sequenceDepth, double sourceSec, QSize sourceSize)
{
    // V1 of the top-level sequence only (the preview binds V1's keys alone).
    if (frame.isNull() || sequenceDepth != 0 || trackIdx != 0
        || clip.stabilizerKeyframes.isEmpty())
        return frame;
    const StabSample sample =
        sampleAt(clip.stabilizerKeyframes, static_cast<qint64>(sourceSec * 1.0e6));
    if (!sourceSize.isValid())
        sourceSize = frame.size();
    if (isIdentity(sample, sourceSize.width(), sourceSize.height()))
        return frame;
    return warpClampToEdge(frame, inverseTransform(sample, sourceSize.width(), sourceSize.height(),
                                                   frame.width(), frame.height()));
}

} // namespace stabwarp
