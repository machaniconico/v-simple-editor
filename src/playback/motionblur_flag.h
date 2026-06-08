#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGlobal>
#include <QVector>

namespace motionblur {

inline bool enabledFromEnv()
{
    return QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR")) == QStringLiteral("1");
}

inline double shutterAngleFromEnv()
{
    bool ok = false;
    const double value =
        QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR_SHUTTER")).toDouble(&ok);
    return ok ? value : 180.0;
}

inline int sampleCountFromEnv()
{
    bool ok = false;
    const int value =
        QString::fromLatin1(qgetenv("VEDITOR_MOTION_BLUR_SAMPLES")).toInt(&ok);
    return qMax(1, ok ? value : 8);
}

inline QImage averagePremultiplied(const QVector<QImage>& samples)
{
    if (samples.isEmpty() || samples.first().isNull())
        return QImage();

    const QSize size = samples.first().size();
    QVector<QImage> premultiplied;
    premultiplied.reserve(samples.size());
    for (const QImage& sample : samples) {
        if (sample.isNull() || sample.size() != size)
            return QImage();
        premultiplied.append(sample.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }

    QImage averaged(size, QImage::Format_ARGB32_Premultiplied);
    const int n = premultiplied.size();
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
            dst[x] = qRgba(static_cast<int>(r / n),
                           static_cast<int>(g / n),
                           static_cast<int>(b / n),
                           static_cast<int>(a / n));
        }
    }

    return averaged.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace motionblur
