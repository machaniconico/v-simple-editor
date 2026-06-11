#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <cmath>

#include "../Timeline.h"

namespace motionblur {

inline bool enabledFromEnv()
{
    return QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR")) == QStringLiteral("1");
}

inline double shutterAngleFromEnv()
{
    bool ok = false;
    const double parsed =
        QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR_SHUTTER")).toDouble(&ok);
    const double value = ok && std::isfinite(parsed) ? parsed : 180.0;
    return qBound(0.0, value, 720.0);
}

inline int sampleCountFromEnv()
{
    bool ok = false;
    const int value =
        QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR_SAMPLES")).toInt(&ok);
    return qMax(1, ok ? value : 8);
}

inline bool activeForTimeline(const Timeline *timeline, bool envFlag)
{
    if (envFlag)
        return true;
    if (!timeline)
        return false;

    // T5/P2 semantic gate: the existing renderer can only accumulate whole
    // timeline samples. A per-clip opt-in therefore acts as a second activation
    // trigger for the current pass; true selective per-layer blur is left to P3.
    for (const TimelineTrack *track : timeline->videoTracks()) {
        if (!track)
            continue;
        for (const ClipInfo &clip : track->clips()) {
            if (clip.motionBlurEnabled)
                return true;
        }
    }
    return false;
}

inline QImage averagePremultiplied(const QVector<QImage>& samples)
{
    if (samples.isEmpty())
        return QImage();

    QSize size;
    QVector<QImage> validSamples;
    validSamples.reserve(samples.size());
    for (const QImage& sample : samples) {
        if (sample.isNull())
            continue;
        if (size.isEmpty())
            size = sample.size();
        if (sample.size() != size)
            continue;
        validSamples.append(sample);
    }

    if (validSamples.isEmpty())
        return QImage();
    if (validSamples.size() == 1) {
        const QImage& only = validSamples.first();
        return only.format() == QImage::Format_RGBA8888
            ? only
            : only.convertToFormat(QImage::Format_RGBA8888);
    }

    QVector<QImage> premultiplied;
    premultiplied.reserve(validSamples.size());
    for (const QImage& sample : validSamples) {
        premultiplied.append(sample.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    QImage averaged(size, QImage::Format_ARGB32_Premultiplied);
    const int n = premultiplied.size();
    const quint64 half = static_cast<quint64>(n) / 2;
    for (int y = 0; y < size.height(); ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(averaged.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            quint64 a = 0;
            quint64 r = 0;
            quint64 g = 0;
            quint64 b = 0;
            for (const QImage& sample : premultiplied) {
                const QRgb* src = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
                const QRgb px = src[x];
                a += qAlpha(px);
                r += qRed(px);
                g += qGreen(px);
                b += qBlue(px);
            }
            dst[x] = qRgba(static_cast<int>((r + half) / n),
                           static_cast<int>((g + half) / n),
                           static_cast<int>((b + half) / n),
                           static_cast<int>((a + half) / n));
        }
    }

    return averaged.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace motionblur
