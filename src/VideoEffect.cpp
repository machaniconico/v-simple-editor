#include "VideoEffect.h"
#include "EffectParamSchema.h"
#include "FractalNoise.h"
#include <QPainter>
#include <QtMath>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

// --- Helpers ---

static inline int clamp255(int v) { return qBound(0, v, 255); }
static inline int clamp255d(double v) { return qBound(0, static_cast<int>(std::round(v)), 255); }

// --- VideoEffect factory ---

QString VideoEffect::typeName(VideoEffectType t)
{
    switch (t) {
    case VideoEffectType::None:      return "None";
    case VideoEffectType::Blur:      return "Blur";
    case VideoEffectType::Sharpen:   return "Sharpen";
    case VideoEffectType::Mosaic:    return "Mosaic";
    case VideoEffectType::ChromaKey: return "Chroma Key";
    case VideoEffectType::Vignette:  return "Vignette";
    case VideoEffectType::Sepia:     return "Sepia";
    case VideoEffectType::Grayscale: return "Grayscale";
    case VideoEffectType::Invert:    return "Invert";
    case VideoEffectType::Noise:          return "Noise";
    case VideoEffectType::DisplacementMap: return "Displacement Map";
    case VideoEffectType::FractalNoiseGen: return "Fractal Noise";
    }
    return "Unknown";
}

QVector<VideoEffectType> VideoEffect::allTypes()
{
    return { VideoEffectType::Blur, VideoEffectType::Sharpen, VideoEffectType::Mosaic,
             VideoEffectType::ChromaKey, VideoEffectType::Vignette, VideoEffectType::Sepia,
             VideoEffectType::Grayscale, VideoEffectType::Invert, VideoEffectType::Noise,
             VideoEffectType::DisplacementMap, VideoEffectType::FractalNoiseGen };
}

VideoEffect VideoEffect::createBlur(double r)
    { VideoEffect e; e.type = VideoEffectType::Blur; e.param1 = r; return e; }
VideoEffect VideoEffect::createSharpen(double a)
    { VideoEffect e; e.type = VideoEffectType::Sharpen; e.param1 = a; return e; }
VideoEffect VideoEffect::createMosaic(double b)
    { VideoEffect e; e.type = VideoEffectType::Mosaic; e.param1 = b; return e; }
VideoEffect VideoEffect::createChromaKey(QColor c, double tol, double soft)
    { VideoEffect e; e.type = VideoEffectType::ChromaKey; e.keyColor = c; e.param1 = tol; e.param2 = soft; return e; }
VideoEffect VideoEffect::createVignette(double i, double r)
    { VideoEffect e; e.type = VideoEffectType::Vignette; e.param1 = i; e.param2 = r; return e; }
VideoEffect VideoEffect::createSepia(double i)
    { VideoEffect e; e.type = VideoEffectType::Sepia; e.param1 = i; return e; }
VideoEffect VideoEffect::createGrayscale()
    { VideoEffect e; e.type = VideoEffectType::Grayscale; return e; }
VideoEffect VideoEffect::createInvert()
    { VideoEffect e; e.type = VideoEffectType::Invert; return e; }
VideoEffect VideoEffect::createNoise(double a)
    { VideoEffect e; e.type = VideoEffectType::Noise; e.param1 = a; return e; }
VideoEffect VideoEffect::createDisplacementMap(double h, double v, int m)
    { VideoEffect e; e.type = VideoEffectType::DisplacementMap; e.param1 = h; e.param2 = v; e.param3 = static_cast<double>(m); return e; }
VideoEffect VideoEffect::createFractalNoise(double s, int o, double evol)
    { VideoEffect effect; effect.type = VideoEffectType::FractalNoiseGen; effect.param1 = s; effect.param2 = static_cast<double>(o); effect.param3 = evol; return effect; }

// ===== EffectParamSchema helper accessors =====

namespace effectctrl {

double paramValue(const VideoEffect &effect, const QString &paramName)
{
    auto schema = paramSchemaFor(effect.type);
    for (const auto &def : schema) {
        if (def.name == paramName) {
            if (def.type == ParamType::Color) return 0.0;
            if (paramName == "radius" && effect.type == VideoEffectType::Blur)
                return effect.param1;
            if (paramName == "amount" && effect.type == VideoEffectType::Sharpen)
                return effect.param1;
            if (paramName == "blockSize" && effect.type == VideoEffectType::Mosaic)
                return effect.param1;
            if (paramName == "tolerance" && effect.type == VideoEffectType::ChromaKey)
                return effect.param1;
            if (paramName == "softness" && effect.type == VideoEffectType::ChromaKey)
                return effect.param2;
            if (paramName == "intensity" && effect.type == VideoEffectType::Vignette)
                return effect.param1;
            if (paramName == "radius" && effect.type == VideoEffectType::Vignette)
                return effect.param2;
            if (paramName == "intensity" && effect.type == VideoEffectType::Sepia)
                return effect.param1;
            if (paramName == "amount" && effect.type == VideoEffectType::Noise)
                return effect.param1;
            return def.defaultVal;
        }
    }
    return 0.0;
}

void setParamValue(VideoEffect &effect, const QString &paramName, double value)
{
    auto schema = paramSchemaFor(effect.type);
    for (const auto &def : schema) {
        if (def.name == paramName) {
            if (def.type == ParamType::Color) return;
            if (paramName == "radius" && effect.type == VideoEffectType::Blur) {
                effect.param1 = value; return;
            }
            if (paramName == "amount" && effect.type == VideoEffectType::Sharpen) {
                effect.param1 = value; return;
            }
            if (paramName == "blockSize" && effect.type == VideoEffectType::Mosaic) {
                effect.param1 = value; return;
            }
            if (paramName == "tolerance" && effect.type == VideoEffectType::ChromaKey) {
                effect.param1 = value; return;
            }
            if (paramName == "softness" && effect.type == VideoEffectType::ChromaKey) {
                effect.param2 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::Vignette) {
                effect.param1 = value; return;
            }
            if (paramName == "radius" && effect.type == VideoEffectType::Vignette) {
                effect.param2 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::Sepia) {
                effect.param1 = value; return;
            }
            if (paramName == "amount" && effect.type == VideoEffectType::Noise) {
                effect.param1 = value; return;
            }
            return;
        }
    }
}

QColor colorParamValue(const VideoEffect &effect, const QString &paramName)
{
    if (paramName == "color" && effect.type == VideoEffectType::ChromaKey)
        return effect.keyColor;
    return QColor();
}

void setColorParam(VideoEffect &effect, const QString &paramName, QColor color)
{
    if (paramName == "color" && effect.type == VideoEffectType::ChromaKey)
        effect.keyColor = color;
}

} // namespace effectctrl

// ===== Color Correction Processing =====

QImage VideoEffectProcessor::applyColorCorrection(const QImage &input, const ColorCorrection &cc)
{
    if (cc.isDefault()) return input;
    QImage img = input.convertToFormat(QImage::Format_RGB888);

    if (cc.exposure != 0.0)
        adjustExposure(img, cc.exposure);
    if (cc.brightness != 0.0 || cc.contrast != 0.0)
        adjustBrightnessContrast(img, cc.brightness, cc.contrast);
    if (cc.highlights != 0.0 || cc.shadows != 0.0)
        adjustHighlightsShadows(img, cc.highlights, cc.shadows);
    if (cc.saturation != 0.0)
        adjustSaturation(img, cc.saturation);
    if (cc.hue != 0.0)
        adjustHue(img, cc.hue);
    if (cc.temperature != 0.0 || cc.tint != 0.0)
        adjustTemperatureTint(img, cc.temperature, cc.tint);
    if (cc.gamma != 1.0)
        adjustGamma(img, cc.gamma);

    // Lift/Gamma/Gain (DaVinci Resolve style) — must match GLSL applyLiftGammaGain()
    bool hasLGG = cc.liftR != 0.0 || cc.liftG != 0.0 || cc.liftB != 0.0
               || cc.gammaR != 0.0 || cc.gammaG != 0.0 || cc.gammaB != 0.0
               || cc.gainR != 0.0 || cc.gainG != 0.0 || cc.gainB != 0.0;
    if (hasLGG) {
        const int w = img.width(), h = img.height();
        for (int y = 0; y < h; ++y) {
            uint8_t *line = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                double r = line[x * 3 + 0] / 255.0;
                double g = line[x * 3 + 1] / 255.0;
                double b = line[x * 3 + 2] / 255.0;

                // Stage 1 — Lift: additive offset (matches GLSL: color + uLift where uLift = liftR*0.5)
                r += cc.liftR * 0.5;
                g += cc.liftG * 0.5;
                b += cc.liftB * 0.5;

                // Stage 2 — Gamma: power curve; internalGamma = pow(2, gammaR) — matches GPU contract
                // cc.gammaR in [-1,1], neutral=0 → internalGamma=1 → identity pow(in,1)
                double igR = std::max(std::pow(2.0, cc.gammaR), 1e-3);
                double igG = std::max(std::pow(2.0, cc.gammaG), 1e-3);
                double igB = std::max(std::pow(2.0, cc.gammaB), 1e-3);
                r = std::pow(std::max(r, 0.0), 1.0 / igR);
                g = std::pow(std::max(g, 0.0), 1.0 / igG);
                b = std::pow(std::max(b, 0.0), 1.0 / igB);

                // Stage 3 — Gain: multiplicative scaling; matches GPU pow(2, gainR*2)
                // cc.gainR in [-1,1], neutral=0 → factor=1 → identity
                r *= std::pow(2.0, cc.gainR * 2.0);
                g *= std::pow(2.0, cc.gainG * 2.0);
                b *= std::pow(2.0, cc.gainB * 2.0);

                line[x * 3 + 0] = static_cast<uint8_t>(qBound(0.0, r, 1.0) * 255.0);
                line[x * 3 + 1] = static_cast<uint8_t>(qBound(0.0, g, 1.0) * 255.0);
                line[x * 3 + 2] = static_cast<uint8_t>(qBound(0.0, b, 1.0) * 255.0);
            }
        }
    }

    return img;
}

void VideoEffectProcessor::adjustBrightnessContrast(QImage &img, double brightness, double contrast)
{
    uint8_t lut[256];
    double bFactor = brightness * 2.55;
    double cFactor = (100.0 + contrast) / 100.0;
    cFactor *= cFactor;

    for (int i = 0; i < 256; ++i) {
        double v = i + bFactor;
        v = ((v / 255.0 - 0.5) * cFactor + 0.5) * 255.0;
        lut[i] = static_cast<uint8_t>(clamp255(static_cast<int>(v)));
    }

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        int bytes = img.width() * 3;
        for (int x = 0; x < bytes; ++x)
            line[x] = lut[line[x]];
    }
}

void VideoEffectProcessor::adjustSaturation(QImage &img, double saturation)
{
    double factor = (saturation + 100.0) / 100.0;
    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            int r = line[idx], g = line[idx + 1], b = line[idx + 2];
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            line[idx]     = clamp255d(lum + factor * (r - lum));
            line[idx + 1] = clamp255d(lum + factor * (g - lum));
            line[idx + 2] = clamp255d(lum + factor * (b - lum));
        }
    }
}

void VideoEffectProcessor::adjustHue(QImage &img, double hue)
{
    double rad = hue * M_PI / 180.0;
    double cosA = std::cos(rad);
    double sinA = std::sin(rad);

    // Rotation matrix for hue shift in RGB space (Pregibon method)
    double m00 = 0.213 + 0.787 * cosA - 0.213 * sinA;
    double m01 = 0.213 - 0.213 * cosA + 0.143 * sinA;
    double m02 = 0.213 - 0.213 * cosA - 0.787 * sinA;
    double m10 = 0.715 - 0.715 * cosA - 0.715 * sinA;
    double m11 = 0.715 + 0.285 * cosA + 0.140 * sinA;
    double m12 = 0.715 - 0.715 * cosA + 0.715 * sinA;
    double m20 = 0.072 - 0.072 * cosA + 0.928 * sinA;
    double m21 = 0.072 - 0.072 * cosA - 0.283 * sinA;
    double m22 = 0.072 + 0.928 * cosA + 0.072 * sinA;

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            double r = line[idx], g = line[idx + 1], b = line[idx + 2];
            line[idx]     = clamp255d(m00 * r + m10 * g + m20 * b);
            line[idx + 1] = clamp255d(m01 * r + m11 * g + m21 * b);
            line[idx + 2] = clamp255d(m02 * r + m12 * g + m22 * b);
        }
    }
}

void VideoEffectProcessor::adjustTemperatureTint(QImage &img, double temperature, double tint)
{
    double rShift = temperature * 0.5;   // warm = +red
    double bShift = -temperature * 0.5;  // warm = -blue
    double gShift = -tint * 0.3;         // +tint = -green (magenta)
    double mShift = tint * 0.2;          // +tint = +red/blue (magenta)

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            line[idx]     = clamp255(line[idx]     + static_cast<int>(rShift + mShift));
            line[idx + 1] = clamp255(line[idx + 1] + static_cast<int>(gShift));
            line[idx + 2] = clamp255(line[idx + 2] + static_cast<int>(bShift + mShift));
        }
    }
}

void VideoEffectProcessor::adjustGamma(QImage &img, double gamma)
{
    uint8_t lut[256];
    double invGamma = 1.0 / gamma;
    for (int i = 0; i < 256; ++i)
        lut[i] = clamp255(static_cast<int>(std::pow(i / 255.0, invGamma) * 255.0));

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        int bytes = img.width() * 3;
        for (int x = 0; x < bytes; ++x)
            line[x] = lut[line[x]];
    }
}

void VideoEffectProcessor::adjustHighlightsShadows(QImage &img, double highlights, double shadows)
{
    double hFactor = highlights / 100.0;
    double sFactor = shadows / 100.0;

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            int r = line[idx], g = line[idx + 1], b = line[idx + 2];
            double lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;

            // Smooth weight: highlights affect bright pixels, shadows affect dark
            double hWeight = lum * lum;            // stronger for brighter
            double sWeight = (1.0 - lum) * (1.0 - lum); // stronger for darker

            double adjust = hFactor * hWeight * 100.0 + sFactor * sWeight * 100.0;
            line[idx]     = clamp255(r + static_cast<int>(adjust));
            line[idx + 1] = clamp255(g + static_cast<int>(adjust));
            line[idx + 2] = clamp255(b + static_cast<int>(adjust));
        }
    }
}

void VideoEffectProcessor::adjustExposure(QImage &img, double exposure)
{
    double factor = std::pow(2.0, exposure);
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = clamp255(static_cast<int>(i * factor));

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        int bytes = img.width() * 3;
        for (int x = 0; x < bytes; ++x)
            line[x] = lut[line[x]];
    }
}

// ===== Video Effect Processing =====

QImage VideoEffectProcessor::applyEffect(const QImage &input, const VideoEffect &effect)
{
    if (!effect.enabled || effect.type == VideoEffectType::None) return input;

    switch (effect.type) {
    case VideoEffectType::Blur:      return applyBlur(input, effect.param1);
    case VideoEffectType::Sharpen:   return applySharpen(input, effect.param1);
    case VideoEffectType::Mosaic:    return applyMosaic(input, effect.param1);
    case VideoEffectType::ChromaKey: return applyChromaKey(input, effect.keyColor, effect.param1, effect.param2);
    case VideoEffectType::Vignette:  return applyVignette(input, effect.param1, effect.param2);
    case VideoEffectType::Sepia:     return applySepia(input, effect.param1);
    case VideoEffectType::Grayscale: return applyGrayscale(input);
    case VideoEffectType::Invert:    return applyInvert(input);
    case VideoEffectType::Noise:          return applyNoise(input, effect.param1);
    case VideoEffectType::DisplacementMap: return applyDisplacementMap(input, effect.param1, effect.param2, static_cast<int>(effect.param3));
    case VideoEffectType::FractalNoiseGen: return applyFractalNoise(input, effect.param1, static_cast<int>(effect.param2), effect.param3);
    default: return input;
    }
}

QImage VideoEffectProcessor::applyEffectStack(const QImage &input, const ColorCorrection &cc,
                                               const QVector<VideoEffect> &effects)
{
    QImage result = applyColorCorrection(input, cc);
    for (const auto &effect : effects)
        result = applyEffect(result, effect);
    return result;
}

// --- Box blur (applied N times for Gaussian approximation) ---
QImage VideoEffectProcessor::applyBlur(const QImage &input, double radius)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int r = qMax(1, static_cast<int>(radius));
    int w = img.width(), h = img.height();
    QImage tmp(w, h, QImage::Format_RGB888);

    // 3-pass box blur approximates Gaussian
    for (int pass = 0; pass < 3; ++pass) {
        // Horizontal pass
        for (int y = 0; y < h; ++y) {
            const uint8_t *src = img.constScanLine(y);
            uint8_t *dst = tmp.scanLine(y);
            for (int x = 0; x < w; ++x) {
                int rSum = 0, gSum = 0, bSum = 0, count = 0;
                for (int kx = -r; kx <= r; ++kx) {
                    int sx = qBound(0, x + kx, w - 1);
                    rSum += src[sx * 3];
                    gSum += src[sx * 3 + 1];
                    bSum += src[sx * 3 + 2];
                    ++count;
                }
                dst[x * 3]     = static_cast<uint8_t>(rSum / count);
                dst[x * 3 + 1] = static_cast<uint8_t>(gSum / count);
                dst[x * 3 + 2] = static_cast<uint8_t>(bSum / count);
            }
        }
        // Vertical pass
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                int rSum = 0, gSum = 0, bSum = 0, count = 0;
                for (int ky = -r; ky <= r; ++ky) {
                    int sy = qBound(0, y + ky, h - 1);
                    const uint8_t *srcLine = tmp.constScanLine(sy);
                    rSum += srcLine[x * 3];
                    gSum += srcLine[x * 3 + 1];
                    bSum += srcLine[x * 3 + 2];
                    ++count;
                }
                uint8_t *dstLine = img.scanLine(y);
                dstLine[x * 3]     = static_cast<uint8_t>(rSum / count);
                dstLine[x * 3 + 1] = static_cast<uint8_t>(gSum / count);
                dstLine[x * 3 + 2] = static_cast<uint8_t>(bSum / count);
            }
        }
    }
    return img;
}

QImage VideoEffectProcessor::applySharpen(const QImage &input, double amount)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    QImage result = img.copy();
    int w = img.width(), h = img.height();

    // Unsharp mask: original + amount * (original - blur)
    for (int y = 1; y < h - 1; ++y) {
        const uint8_t *prev = img.constScanLine(y - 1);
        const uint8_t *curr = img.constScanLine(y);
        const uint8_t *next = img.constScanLine(y + 1);
        uint8_t *dst = result.scanLine(y);

        for (int x = 1; x < w - 1; ++x) {
            for (int c = 0; c < 3; ++c) {
                int idx = x * 3 + c;
                // 3x3 Laplacian kernel center
                int center = curr[idx] * 5
                    - prev[idx] - next[idx]
                    - curr[(x - 1) * 3 + c] - curr[(x + 1) * 3 + c];
                double v = curr[idx] + amount * (center - curr[idx]) * 0.25;
                dst[idx] = clamp255d(v);
            }
        }
    }
    return result;
}

QImage VideoEffectProcessor::applyMosaic(const QImage &input, double blockSize)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int bs = qMax(2, static_cast<int>(blockSize));
    int w = img.width(), h = img.height();

    for (int by = 0; by < h; by += bs) {
        for (int bx = 0; bx < w; bx += bs) {
            int rSum = 0, gSum = 0, bSum = 0, count = 0;
            int endX = qMin(bx + bs, w);
            int endY = qMin(by + bs, h);

            for (int y = by; y < endY; ++y) {
                const uint8_t *line = img.constScanLine(y);
                for (int x = bx; x < endX; ++x) {
                    rSum += line[x * 3];
                    gSum += line[x * 3 + 1];
                    bSum += line[x * 3 + 2];
                    ++count;
                }
            }

            uint8_t avgR = rSum / count, avgG = gSum / count, avgB = bSum / count;
            for (int y = by; y < endY; ++y) {
                uint8_t *line = img.scanLine(y);
                for (int x = bx; x < endX; ++x) {
                    line[x * 3] = avgR;
                    line[x * 3 + 1] = avgG;
                    line[x * 3 + 2] = avgB;
                }
            }
        }
    }
    return img;
}

QImage VideoEffectProcessor::applyChromaKey(const QImage &input, QColor keyColor,
                                             double tolerance, double softness)
{
    QImage img = input.convertToFormat(QImage::Format_ARGB32);
    int kr = keyColor.red(), kg = keyColor.green(), kb = keyColor.blue();

    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            double dist = std::sqrt((r - kr) * (r - kr) + (g - kg) * (g - kg) + (b - kb) * (b - kb));

            if (dist < tolerance) {
                line[x] = qRgba(r, g, b, 0); // fully transparent
            } else if (dist < tolerance + softness) {
                double alpha = (dist - tolerance) / softness;
                line[x] = qRgba(r, g, b, static_cast<int>(alpha * 255));
            }
        }
    }
    return img;
}

QImage VideoEffectProcessor::applyVignette(const QImage &input, double intensity, double radius)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    int w = img.width(), h = img.height();
    double cx = w * 0.5, cy = h * 0.5;
    double maxDist = std::sqrt(cx * cx + cy * cy);

    for (int y = 0; y < h; ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double dist = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            double normDist = dist / maxDist;
            double vignette = 1.0;
            if (normDist > radius) {
                double t = (normDist - radius) / (1.0 - radius);
                vignette = 1.0 - intensity * t * t;
                vignette = qMax(0.0, vignette);
            }
            int idx = x * 3;
            line[idx]     = clamp255d(line[idx]     * vignette);
            line[idx + 1] = clamp255d(line[idx + 1] * vignette);
            line[idx + 2] = clamp255d(line[idx + 2] * vignette);
        }
    }
    return img;
}

QImage VideoEffectProcessor::applySepia(const QImage &input, double intensity)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            int r = line[idx], g = line[idx + 1], b = line[idx + 2];

            int sr = clamp255(static_cast<int>(r * 0.393 + g * 0.769 + b * 0.189));
            int sg = clamp255(static_cast<int>(r * 0.349 + g * 0.686 + b * 0.168));
            int sb = clamp255(static_cast<int>(r * 0.272 + g * 0.534 + b * 0.131));

            line[idx]     = clamp255d(r + intensity * (sr - r));
            line[idx + 1] = clamp255d(g + intensity * (sg - g));
            line[idx + 2] = clamp255d(b + intensity * (sb - b));
        }
    }
    return img;
}

QImage VideoEffectProcessor::applyGrayscale(const QImage &input)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            int idx = x * 3;
            uint8_t gray = static_cast<uint8_t>(
                0.2126 * line[idx] + 0.7152 * line[idx + 1] + 0.0722 * line[idx + 2]);
            line[idx] = line[idx + 1] = line[idx + 2] = gray;
        }
    }
    return img;
}

QImage VideoEffectProcessor::applyInvert(const QImage &input)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        int bytes = img.width() * 3;
        for (int x = 0; x < bytes; ++x)
            line[x] = 255 - line[x];
    }
    return img;
}

QImage VideoEffectProcessor::applyNoise(const QImage &input, double amount)
{
    QImage img = input.convertToFormat(QImage::Format_RGB888);
    auto *rng = QRandomGenerator::global();
    int a = static_cast<int>(amount);

    for (int y = 0; y < img.height(); ++y) {
        uint8_t *line = img.scanLine(y);
        for (int x = 0; x < img.width() * 3; ++x) {
            int noise = static_cast<int>(rng->bounded(2 * a + 1)) - a;
            line[x] = clamp255(line[x] + noise);
        }
    }
    return img;
}

QImage VideoEffectProcessor::applyDisplacementMap(const QImage &input, double hAmt, double vAmt, int mode)
{
    QImage src = input.convertToFormat(QImage::Format_RGB888);
    int w = src.width(), h = src.height();
    QImage result(w, h, QImage::Format_RGB888);

    // Build or obtain the displacement map
    QImage mapImg;
    if (mode == 1) {
        // Use the image's own luminance as the map
        mapImg = src.copy();
        for (int y = 0; y < h; ++y) {
            uint8_t *line = mapImg.scanLine(y);
            for (int x = 0; x < w; ++x) {
                int idx = x * 3;
                uint8_t lum = static_cast<uint8_t>(
                    0.2126 * line[idx] + 0.7152 * line[idx + 1] + 0.0722 * line[idx + 2]);
                line[idx] = line[idx + 1] = line[idx + 2] = lum;
            }
        }
    } else {
        // Generate procedural fractal noise map (mode == 0)
        FractalNoise::Params p;
        p.kind = FractalNoise::FractalKind::FBm;
        p.octaves = 5;
        p.lacunarity = 2.0;
        p.gain = 0.5;
        p.frequency = 4.0;
        p.seed = 1337;
        mapImg = FractalNoise::render(QSize(w, h), 0.0, p, true);
        // Convert grayscale8 to RGB888 for uniform access below
        if (mapImg.format() != QImage::Format_RGB888)
            mapImg = mapImg.convertToFormat(QImage::Format_RGB888);
    }

    // Identity fast-path
    if (hAmt == 0.0 && vAmt == 0.0)
        return src.copy();

    for (int y = 0; y < h; ++y) {
        const uint8_t *srcLine = src.constScanLine(y);
        const uint8_t *mapLine = mapImg.constScanLine(y);
        uint8_t *dstLine = result.scanLine(y);

        for (int x = 0; x < w; ++x) {
            // Read map value (use red channel; map is grayscale so all channels equal)
            double m = mapLine[x * 3] / 255.0;   // 0..1
            m = m * 2.0 - 1.0;                     // -1..1

            // Compute displaced source coordinates
            int sx = qBound(0, static_cast<int>(std::round(x + m * hAmt)), w - 1);
            int sy = qBound(0, static_cast<int>(std::round(y + m * vAmt)), h - 1);

            // Sample source at displaced position (nearest-neighbor)
            const uint8_t *srcSy = src.constScanLine(sy);
            dstLine[x * 3]     = srcSy[sx * 3];
            dstLine[x * 3 + 1] = srcSy[sx * 3 + 1];
            dstLine[x * 3 + 2] = srcSy[sx * 3 + 2];
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyFractalNoise(const QImage &input, double scale, int octaves, double evolution)
{
    // FractalNoiseGen is a generator: it replaces the input with a fractal noise image
    // of the same dimensions.  The input image is used only for its size.
    int w = input.width(), h = input.height();

    FractalNoise::Params p;
    p.kind = FractalNoise::FractalKind::FBm;
    p.octaves = qBound(1, octaves, 8);
    p.lacunarity = 2.0;
    p.gain = 0.5;
    p.frequency = scale;
    p.seed = 1337;

    // Render as grayscale (matches FractalNoise::render output)
    QImage noiseImg = FractalNoise::render(QSize(w, h), evolution, p, true);
    return noiseImg.convertToFormat(QImage::Format_RGB888);
}
