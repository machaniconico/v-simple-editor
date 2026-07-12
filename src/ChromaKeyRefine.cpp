#include "ChromaKeyRefine.h"
#include <cmath>

namespace chromakey {

// --- internal helpers ---

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// GLSL-style smoothstep
static double smoothstep(double e0, double e1, double x)
{
    double range = e1 - e0;
    // guard zero-width band
    if (range <= 0.0)
        return x <= e0 ? 0.0 : 1.0;
    double t = clamp01((x - e0) / range);
    return t * t * (3.0 - 2.0 * t);
}

// HSV-distance between two colours (normalised hue+sat, ignoring value)
static double hsvDistance(const QColor &a, const QColor &b)
{
    float ahf, asf, avf, bhf, bsf, bvf;
    a.getHsvF(&ahf, &asf, &avf);
    b.getHsvF(&bhf, &bsf, &bvf);
    double ah = ahf, as_ = asf, bh = bhf, bs = bsf;
    if (ah < 0.0) ah = 0.0;
    if (bh < 0.0) bh = 0.0;

    // Hue is circular; compute shortest arc [0, 0.5]
    double dh = std::abs(ah - bh);
    if (dh > 0.5)
        dh = 1.0 - dh;

    double ds = std::abs(as_ - bs);

    // Combine: weight hue more heavily for typical chroma keys
    return clamp01(dh * 1.5 + ds * 0.5);
}

// --- public API ---

double computeAlpha(const QColor &px, const KeyConfig &cfg)
{
    double d = hsvDistance(px, cfg.keyColor);

    // smoothness must be positive to avoid degenerate band
    double smooth = cfg.smoothness > 0.0 ? cfg.smoothness : 1e-6;

    double e0 = cfg.similarity - smooth;
    double e1 = cfg.similarity + smooth;

    // alpha = 0 when d <= e0 (matches key), = 1 when d >= e1 (far from key)
    return smoothstep(e0, e1, d);
}

QColor despill(const QColor &px, const KeyConfig &cfg)
{
    if (cfg.spillSuppress <= 0.0)
        return px;

    double r = px.redF();
    double g = px.greenF();
    double b = px.blueF();

    double kr = cfg.keyColor.redF();
    double kg = cfg.keyColor.greenF();
    double kb = cfg.keyColor.blueF();

    // Determine dominant key channel
    if (kg >= kr && kg >= kb) {
        // Green-dominant key: suppress green toward (R+B)/2
        double target = (r + b) * 0.5;
        double suppressed = g + (target - g) * cfg.spillSuppress;
        g = clamp01(suppressed);
    } else if (kb >= kr && kb >= kg) {
        // Blue-dominant key
        double target = (r + g) * 0.5;
        double suppressed = b + (target - b) * cfg.spillSuppress;
        b = clamp01(suppressed);
    } else {
        // Red-dominant key
        double target = (g + b) * 0.5;
        double suppressed = r + (target - r) * cfg.spillSuppress;
        r = clamp01(suppressed);
    }

    QColor result;
    result.setRgbF(r, g, b);
    return result;
}

QImage refineMatte(const QImage &source, const KeyConfig &cfg)
{
    QImage src = source.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.width(), src.height(), QImage::Format_ARGB32);

    for (int y = 0; y < src.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb       *dstLine = reinterpret_cast<QRgb *>(out.scanLine(y));

        for (int x = 0; x < src.width(); ++x) {
            QColor px(srcLine[x]);

            double alpha  = computeAlpha(px, cfg);
            QColor result = despill(px, cfg);

            int a8 = static_cast<int>(std::round(alpha * 255.0));
            dstLine[x] = qRgba(result.red(), result.green(), result.blue(), a8);
        }
    }

    return out;
}

} // namespace chromakey
