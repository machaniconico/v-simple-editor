#include "AIMask.h"

#include <algorithm>
#include <cmath>
#include <QColor>

namespace aimask {

// ---------------------------------------------------------------------------
// availableEngines
// ---------------------------------------------------------------------------
QStringList availableEngines()
{
    return { QStringLiteral("LumaThreshold"),
             QStringLiteral("ColorRange"),
             QStringLiteral("ExternalPlugin") };
}

// ---------------------------------------------------------------------------
// engineToString / engineFromString
// ---------------------------------------------------------------------------
QString engineToString(Engine engine)
{
    switch (engine) {
    case Engine::LumaThreshold:  return QStringLiteral("LumaThreshold");
    case Engine::ColorRange:     return QStringLiteral("ColorRange");
    case Engine::ExternalPlugin: return QStringLiteral("ExternalPlugin");
    }
    return QStringLiteral("LumaThreshold");
}

Engine engineFromString(const QString& name)
{
    if (name == QStringLiteral("ColorRange"))     return Engine::ColorRange;
    if (name == QStringLiteral("ExternalPlugin")) return Engine::ExternalPlugin;
    return Engine::LumaThreshold;
}

// ---------------------------------------------------------------------------
// Helper: return a zero-filled Grayscale8 image of the given size
// ---------------------------------------------------------------------------
static QImage zeroMask(int w, int h)
{
    QImage m(w, h, QImage::Format_Grayscale8);
    m.fill(0);
    return m;
}

// ---------------------------------------------------------------------------
// lumaThresholdMask
// ---------------------------------------------------------------------------
static MaskResult lumaThresholdMask(const QImage& src, double threshold)
{
    // Convert to ARGB32 for uniform pixel access
    const QImage in = (src.format() == QImage::Format_ARGB32)
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32);

    const int w = in.width();
    const int h = in.height();
    QImage mask(w, h, QImage::Format_Grayscale8);

    for (int y = 0; y < h; ++y) {
        const QRgb*    srcLine  = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        unsigned char* maskLine = mask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            // qGray returns 0..255; normalise to 0..1 for threshold compare
            const double luma = qGray(srcLine[x]) / 255.0;
            maskLine[x] = (luma >= threshold) ? 255 : 0;
        }
    }

    return { mask, true, QString() };
}

// ---------------------------------------------------------------------------
// colorRangeMask
// ---------------------------------------------------------------------------
static MaskResult colorRangeMask(const QImage& src,
                                  const QColor& target,
                                  double tolerance)
{
    const QImage in = (src.format() == QImage::Format_ARGB32)
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32);

    const int w = in.width();
    const int h = in.height();
    QImage mask(w, h, QImage::Format_Grayscale8);

    // Normalise target HSV to [0..1]
    const double tH = target.hsvHueF();      // -1 if achromatic
    const double tS = target.hsvSaturationF();
    const double tV = target.valueF();

    for (int y = 0; y < h; ++y) {
        const QRgb*    srcLine  = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        unsigned char* maskLine = mask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const QColor c(srcLine[x]);
            const double pH = c.hsvHueF();
            const double pS = c.hsvSaturationF();
            const double pV = c.valueF();

            // Hue distance with wrap-around (both achromatic → distance 0)
            double dH = 0.0;
            if (tH >= 0.0 && pH >= 0.0) {
                dH = std::fabs(pH - tH);
                if (dH > 0.5) dH = 1.0 - dH; // wrap
            } else if (tH >= 0.0 || pH >= 0.0) {
                dH = 0.5; // one is achromatic, other is not → moderate distance
            }

            const double dS = std::fabs(pS - tS);
            const double dV = std::fabs(pV - tV);

            // Average distance [0..1]
            const double dist = (dH + dS + dV) / 3.0;
            maskLine[x] = (dist <= tolerance) ? 255 : 0;
        }
    }

    return { mask, true, QString() };
}

// ---------------------------------------------------------------------------
// generateMask
// ---------------------------------------------------------------------------
MaskResult generateMask(const QImage& source, const MaskParams& params)
{
    // Null source guard
    if (source.isNull()) {
        MaskResult r;
        r.mask    = zeroMask(1, 1);
        r.success = false;
        r.error   = QStringLiteral("source image is null");
        return r;
    }

    switch (params.engine) {
    case Engine::LumaThreshold:
        return lumaThresholdMask(source, params.lumaThreshold);

    case Engine::ColorRange:
        return colorRangeMask(source, params.colorTarget, params.colorTolerance);

    case Engine::ExternalPlugin: {
        // External plugin support deferred
        MaskResult r;
        r.mask    = zeroMask(source.width(), source.height());
        r.success = false;
        r.error   = QStringLiteral("ExternalPlugin engine not yet supported");
        return r;
    }
    }

    // Unreachable
    return { zeroMask(source.width(), source.height()), false,
             QStringLiteral("unknown engine") };
}

} // namespace aimask
