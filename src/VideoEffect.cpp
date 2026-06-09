#include "VideoEffect.h"
#include "EffectParamSchema.h"
#include "FractalNoise.h"
#include <QPainter>
#include <QtMath>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <cstdint>

// --- Helpers ---

static inline int clamp255(int v) { return qBound(0, v, 255); }
static inline int clamp255d(double v) { return qBound(0, static_cast<int>(std::round(v)), 255); }
static inline double luma709(int r, int g, int b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
static inline double smoothstep(double t)
{
    t = qBound(0.0, t, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

static inline double rgbSaturation01(int r, int g, int b)
{
    const int maxC = std::max({ r, g, b });
    const int minC = std::min({ r, g, b });
    return static_cast<double>(maxC - minC) / 255.0;
}

static inline QRgb sampleArgbPixel(const QImage &img, int x, int y)
{
    const int sx = qBound(0, x, img.width() - 1);
    const int sy = qBound(0, y, img.height() - 1);
    const QRgb *line = reinterpret_cast<const QRgb*>(img.constScanLine(sy));
    return line[sx];
}

static inline std::uint32_t glitchHash(int row, int seed)
{
    std::uint32_t x = static_cast<std::uint32_t>(row) * 0x45d9f3bu
                    ^ static_cast<std::uint32_t>(seed) * 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline void addRgbSample(const QImage &img, int x, int y,
                                double &rSum, double &gSum, double &bSum)
{
    const int sx = qBound(0, x, img.width() - 1);
    const int sy = qBound(0, y, img.height() - 1);
    const uint8_t *line = img.constScanLine(sy);
    rSum += line[sx * 3];
    gSum += line[sx * 3 + 1];
    bSum += line[sx * 3 + 2];
}

static QVector<double> boxBlurRgbBuffer(const QVector<double> &src, int w, int h, int radius)
{
    if (radius <= 0 || w <= 0 || h <= 0)
        return src;

    QVector<double> tmp(src.size(), 0.0);
    QVector<double> dst(src.size(), 0.0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sums[3] = { 0.0, 0.0, 0.0 };
            int count = 0;
            for (int kx = -radius; kx <= radius; ++kx) {
                const int sx = qBound(0, x + kx, w - 1);
                const int idx = (y * w + sx) * 3;
                sums[0] += src[idx];
                sums[1] += src[idx + 1];
                sums[2] += src[idx + 2];
                ++count;
            }
            const int dstIdx = (y * w + x) * 3;
            tmp[dstIdx] = sums[0] / count;
            tmp[dstIdx + 1] = sums[1] / count;
            tmp[dstIdx + 2] = sums[2] / count;
        }
    }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sums[3] = { 0.0, 0.0, 0.0 };
            int count = 0;
            for (int ky = -radius; ky <= radius; ++ky) {
                const int sy = qBound(0, y + ky, h - 1);
                const int idx = (sy * w + x) * 3;
                sums[0] += tmp[idx];
                sums[1] += tmp[idx + 1];
                sums[2] += tmp[idx + 2];
                ++count;
            }
            const int dstIdx = (y * w + x) * 3;
            dst[dstIdx] = sums[0] / count;
            dst[dstIdx + 1] = sums[1] / count;
            dst[dstIdx + 2] = sums[2] / count;
        }
    }

    return dst;
}

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
    case VideoEffectType::GaussianBlur: return "ガウスブラー";
    case VideoEffectType::DirectionalBlur: return "方向ブラー";
    case VideoEffectType::RadialBlur: return "放射ブラー";
    case VideoEffectType::Glow: return "グロー";
    case VideoEffectType::FindEdges: return "輪郭検出";
    case VideoEffectType::Emboss: return "エンボス";
    case VideoEffectType::Posterize: return "ポスタリゼーション";
    case VideoEffectType::Threshold: return "2階調(しきい値)";
    case VideoEffectType::Solarize: return "ソラリゼーション";
    case VideoEffectType::Levels: return "レベル補正";
    case VideoEffectType::Tint: return "色合い(Tint)";
    case VideoEffectType::BlackWhite: return "白黒";
    case VideoEffectType::Exposure: return "露出";
    case VideoEffectType::HueSaturation: return "色相/彩度";
    case VideoEffectType::RGBSplit: return "RGBスプリット";
    case VideoEffectType::WaveWarp: return "波形ワープ";
    case VideoEffectType::Ripple: return "リップル";
    case VideoEffectType::GlitchVHS: return "グリッチ(VHS)";
    case VideoEffectType::GradientRamp: return "グラデーション";
    case VideoEffectType::Fill: return "塗りつぶし";
    case VideoEffectType::Bloom: return "ブルーム";
    case VideoEffectType::Scanlines: return "走査線(CRT)";
    case VideoEffectType::Halftone: return "ハーフトーン";
    case VideoEffectType::Curves: return "カーブ";
    case VideoEffectType::ChannelMixer: return "チャンネルミキサー";
    case VideoEffectType::Vibrance: return "自然な彩度";
    case VideoEffectType::PhotoFilter: return "フォトフィルター";
    case VideoEffectType::Tritone: return "トライトーン";
    case VideoEffectType::BrightnessContrast: return "明るさ・コントラスト";
    }
    return "Unknown";
}

QVector<VideoEffectType> VideoEffect::allTypes()
{
    return { VideoEffectType::Blur, VideoEffectType::Sharpen, VideoEffectType::Mosaic,
             VideoEffectType::ChromaKey, VideoEffectType::Vignette, VideoEffectType::Sepia,
             VideoEffectType::Grayscale, VideoEffectType::Invert, VideoEffectType::Noise,
             VideoEffectType::DisplacementMap, VideoEffectType::FractalNoiseGen,
             VideoEffectType::GaussianBlur, VideoEffectType::DirectionalBlur,
             VideoEffectType::RadialBlur, VideoEffectType::Glow,
             VideoEffectType::FindEdges, VideoEffectType::Emboss,
             VideoEffectType::Posterize, VideoEffectType::Threshold,
             VideoEffectType::Solarize, VideoEffectType::Levels,
             VideoEffectType::Tint, VideoEffectType::BlackWhite,
             VideoEffectType::Exposure, VideoEffectType::HueSaturation,
             VideoEffectType::RGBSplit, VideoEffectType::WaveWarp,
             VideoEffectType::Ripple, VideoEffectType::GlitchVHS,
             VideoEffectType::GradientRamp, VideoEffectType::Fill,
             VideoEffectType::Bloom, VideoEffectType::Scanlines,
             VideoEffectType::Halftone, VideoEffectType::Curves,
             VideoEffectType::ChannelMixer, VideoEffectType::Vibrance,
             VideoEffectType::PhotoFilter, VideoEffectType::Tritone,
             VideoEffectType::BrightnessContrast };
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
VideoEffect VideoEffect::createGaussianBlur(double r)
    { VideoEffect e; e.type = VideoEffectType::GaussianBlur; e.param1 = r; return e; }
VideoEffect VideoEffect::createDirectionalBlur(double a, double l)
    { VideoEffect e; e.type = VideoEffectType::DirectionalBlur; e.param1 = a; e.param2 = l; return e; }
VideoEffect VideoEffect::createRadialBlur(double a, int m)
    { VideoEffect e; e.type = VideoEffectType::RadialBlur; e.param1 = a; e.param2 = static_cast<double>(m); return e; }
VideoEffect VideoEffect::createGlow(double t, double r, double i)
    { VideoEffect e; e.type = VideoEffectType::Glow; e.param1 = t; e.param2 = r; e.param3 = i; return e; }
VideoEffect VideoEffect::createFindEdges(double i)
    { VideoEffect e; e.type = VideoEffectType::FindEdges; e.param1 = i; return e; }
VideoEffect VideoEffect::createEmboss(double a, double m)
    { VideoEffect e; e.type = VideoEffectType::Emboss; e.param1 = a; e.param2 = m; return e; }
VideoEffect VideoEffect::createPosterize(double l)
    { VideoEffect e; e.type = VideoEffectType::Posterize; e.param1 = l; return e; }
VideoEffect VideoEffect::createThreshold(double l)
    { VideoEffect e; e.type = VideoEffectType::Threshold; e.param1 = l; return e; }
VideoEffect VideoEffect::createSolarize(double t)
    { VideoEffect e; e.type = VideoEffectType::Solarize; e.param1 = t; return e; }
VideoEffect VideoEffect::createLevels(double b, double w, double g)
    { VideoEffect e; e.type = VideoEffectType::Levels; e.param1 = b; e.param2 = w; e.param3 = g; return e; }
VideoEffect VideoEffect::createTint(double a, QColor c)
    { VideoEffect e; e.type = VideoEffectType::Tint; e.param1 = a; e.keyColor = c; return e; }
VideoEffect VideoEffect::createBlackWhite(double r, double g, double b)
    { VideoEffect e; e.type = VideoEffectType::BlackWhite; e.param1 = r; e.param2 = g; e.param3 = b; return e; }
VideoEffect VideoEffect::createExposure(double s)
    { VideoEffect e; e.type = VideoEffectType::Exposure; e.param1 = s; return e; }
VideoEffect VideoEffect::createHueSaturation(double h, double s, double l)
    { VideoEffect e; e.type = VideoEffectType::HueSaturation; e.param1 = h; e.param2 = s; e.param3 = l; return e; }
VideoEffect VideoEffect::createRGBSplit(double x, double y)
    { VideoEffect e; e.type = VideoEffectType::RGBSplit; e.param1 = x; e.param2 = y; return e; }
VideoEffect VideoEffect::createWaveWarp(double a, double w, double p)
    { VideoEffect e; e.type = VideoEffectType::WaveWarp; e.param1 = a; e.param2 = w; e.param3 = p; return e; }
VideoEffect VideoEffect::createRipple(double a, double w, double p)
    { VideoEffect e; e.type = VideoEffectType::Ripple; e.param1 = a; e.param2 = w; e.param3 = p; return e; }
VideoEffect VideoEffect::createGlitchVHS(double i, double b, double s)
    { VideoEffect e; e.type = VideoEffectType::GlitchVHS; e.param1 = i; e.param2 = b; e.param3 = s; return e; }
VideoEffect VideoEffect::createGradientRamp(int t, double a, double o, QColor c)
    { VideoEffect e; e.type = VideoEffectType::GradientRamp; e.param1 = static_cast<double>(t); e.param2 = a; e.param3 = o; e.keyColor = c; return e; }
VideoEffect VideoEffect::createFill(QColor c, double o)
    { VideoEffect e; e.type = VideoEffectType::Fill; e.param1 = o; e.keyColor = c; return e; }
VideoEffect VideoEffect::createBloom(double t, double r, double i)
    { VideoEffect e; e.type = VideoEffectType::Bloom; e.param1 = t; e.param2 = r; e.param3 = i; return e; }
VideoEffect VideoEffect::createScanlines(double s, double d, double o)
    { VideoEffect e; e.type = VideoEffectType::Scanlines; e.param1 = s; e.param2 = d; e.param3 = o; return e; }
VideoEffect VideoEffect::createHalftone(double s, double a)
    { VideoEffect e; e.type = VideoEffectType::Halftone; e.param1 = s; e.param2 = a; return e; }
VideoEffect VideoEffect::createCurves(double s, double h, double m)
    { VideoEffect e; e.type = VideoEffectType::Curves; e.param1 = s; e.param2 = h; e.param3 = m; return e; }
VideoEffect VideoEffect::createChannelMixer(double r, double g, double b)
    { VideoEffect e; e.type = VideoEffectType::ChannelMixer; e.param1 = r; e.param2 = g; e.param3 = b; return e; }
VideoEffect VideoEffect::createVibrance(double v)
    { VideoEffect e; e.type = VideoEffectType::Vibrance; e.param1 = v; return e; }
VideoEffect VideoEffect::createPhotoFilter(QColor c, double d)
    { VideoEffect e; e.type = VideoEffectType::PhotoFilter; e.keyColor = c; e.param1 = d; return e; }
VideoEffect VideoEffect::createTritone(QColor c, double b)
    { VideoEffect e; e.type = VideoEffectType::Tritone; e.keyColor = c; e.param1 = b; return e; }
VideoEffect VideoEffect::createBrightnessContrast(double b, double c)
    { VideoEffect e; e.type = VideoEffectType::BrightnessContrast; e.param1 = b; e.param2 = c; return e; }

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
            if (paramName == "radius" && effect.type == VideoEffectType::GaussianBlur)
                return effect.param1;
            if (paramName == "angle" && effect.type == VideoEffectType::DirectionalBlur)
                return effect.param1;
            if (paramName == "length" && effect.type == VideoEffectType::DirectionalBlur)
                return effect.param2;
            if (paramName == "amount" && effect.type == VideoEffectType::RadialBlur)
                return effect.param1;
            if (paramName == "mode" && effect.type == VideoEffectType::RadialBlur)
                return effect.param2;
            if (paramName == "threshold" && effect.type == VideoEffectType::Glow)
                return effect.param1;
            if (paramName == "radius" && effect.type == VideoEffectType::Glow)
                return effect.param2;
            if (paramName == "intensity" && effect.type == VideoEffectType::Glow)
                return effect.param3;
            if (paramName == "intensity" && effect.type == VideoEffectType::FindEdges)
                return effect.param1;
            if (paramName == "angle" && effect.type == VideoEffectType::Emboss)
                return effect.param1;
            if (paramName == "amount" && effect.type == VideoEffectType::Emboss)
                return effect.param2;
            if (paramName == "levels" && effect.type == VideoEffectType::Posterize)
                return effect.param1;
            if (paramName == "level" && effect.type == VideoEffectType::Threshold)
                return effect.param1;
            if (paramName == "threshold" && effect.type == VideoEffectType::Solarize)
                return effect.param1;
            if (paramName == "inputBlack" && effect.type == VideoEffectType::Levels)
                return effect.param1;
            if (paramName == "inputWhite" && effect.type == VideoEffectType::Levels)
                return effect.param2;
            if (paramName == "gamma" && effect.type == VideoEffectType::Levels)
                return effect.param3;
            if (paramName == "amount" && effect.type == VideoEffectType::Tint)
                return effect.param1;
            if (paramName == "redWeight" && effect.type == VideoEffectType::BlackWhite)
                return effect.param1;
            if (paramName == "greenWeight" && effect.type == VideoEffectType::BlackWhite)
                return effect.param2;
            if (paramName == "blueWeight" && effect.type == VideoEffectType::BlackWhite)
                return effect.param3;
            if (paramName == "stops" && effect.type == VideoEffectType::Exposure)
                return effect.param1;
            if (paramName == "hueDegrees" && effect.type == VideoEffectType::HueSaturation)
                return effect.param1;
            if (paramName == "saturation" && effect.type == VideoEffectType::HueSaturation)
                return effect.param2;
            if (paramName == "lightness" && effect.type == VideoEffectType::HueSaturation)
                return effect.param3;
            if (paramName == "offsetX" && effect.type == VideoEffectType::RGBSplit)
                return effect.param1;
            if (paramName == "offsetY" && effect.type == VideoEffectType::RGBSplit)
                return effect.param2;
            if (paramName == "amplitude" && effect.type == VideoEffectType::WaveWarp)
                return effect.param1;
            if (paramName == "wavelength" && effect.type == VideoEffectType::WaveWarp)
                return effect.param2;
            if (paramName == "phase" && effect.type == VideoEffectType::WaveWarp)
                return effect.param3;
            if (paramName == "amplitude" && effect.type == VideoEffectType::Ripple)
                return effect.param1;
            if (paramName == "wavelength" && effect.type == VideoEffectType::Ripple)
                return effect.param2;
            if (paramName == "phase" && effect.type == VideoEffectType::Ripple)
                return effect.param3;
            if (paramName == "intensity" && effect.type == VideoEffectType::GlitchVHS)
                return effect.param1;
            if (paramName == "blockHeight" && effect.type == VideoEffectType::GlitchVHS)
                return effect.param2;
            if (paramName == "seed" && effect.type == VideoEffectType::GlitchVHS)
                return effect.param3;
            if (paramName == "type" && effect.type == VideoEffectType::GradientRamp)
                return effect.param1;
            if (paramName == "angle" && effect.type == VideoEffectType::GradientRamp)
                return effect.param2;
            if (paramName == "opacity" && effect.type == VideoEffectType::GradientRamp)
                return effect.param3;
            if (paramName == "opacity" && effect.type == VideoEffectType::Fill)
                return effect.param1;
            if (paramName == "threshold" && effect.type == VideoEffectType::Bloom)
                return effect.param1;
            if (paramName == "radius" && effect.type == VideoEffectType::Bloom)
                return effect.param2;
            if (paramName == "intensity" && effect.type == VideoEffectType::Bloom)
                return effect.param3;
            if (paramName == "lineSpacing" && effect.type == VideoEffectType::Scanlines)
                return effect.param1;
            if (paramName == "darkness" && effect.type == VideoEffectType::Scanlines)
                return effect.param2;
            if (paramName == "opacity" && effect.type == VideoEffectType::Scanlines)
                return effect.param3;
            if (paramName == "dotSize" && effect.type == VideoEffectType::Halftone)
                return effect.param1;
            if (paramName == "angle" && effect.type == VideoEffectType::Halftone)
                return effect.param2;
            if (paramName == "shadows" && effect.type == VideoEffectType::Curves)
                return effect.param1;
            if (paramName == "highlights" && effect.type == VideoEffectType::Curves)
                return effect.param2;
            if (paramName == "midContrast" && effect.type == VideoEffectType::Curves)
                return effect.param3;
            if (paramName == "redFromRed" && effect.type == VideoEffectType::ChannelMixer)
                return effect.param1;
            if (paramName == "greenFromGreen" && effect.type == VideoEffectType::ChannelMixer)
                return effect.param2;
            if (paramName == "blueFromBlue" && effect.type == VideoEffectType::ChannelMixer)
                return effect.param3;
            if (paramName == "vibrance" && effect.type == VideoEffectType::Vibrance)
                return effect.param1;
            if (paramName == "density" && effect.type == VideoEffectType::PhotoFilter)
                return effect.param1;
            if (paramName == "blend" && effect.type == VideoEffectType::Tritone)
                return effect.param1;
            if (paramName == "brightness" && effect.type == VideoEffectType::BrightnessContrast)
                return effect.param1;
            if (paramName == "contrast" && effect.type == VideoEffectType::BrightnessContrast)
                return effect.param2;
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
            if (paramName == "radius" && effect.type == VideoEffectType::GaussianBlur) {
                effect.param1 = value; return;
            }
            if (paramName == "angle" && effect.type == VideoEffectType::DirectionalBlur) {
                effect.param1 = value; return;
            }
            if (paramName == "length" && effect.type == VideoEffectType::DirectionalBlur) {
                effect.param2 = value; return;
            }
            if (paramName == "amount" && effect.type == VideoEffectType::RadialBlur) {
                effect.param1 = value; return;
            }
            if (paramName == "mode" && effect.type == VideoEffectType::RadialBlur) {
                effect.param2 = value; return;
            }
            if (paramName == "threshold" && effect.type == VideoEffectType::Glow) {
                effect.param1 = value; return;
            }
            if (paramName == "radius" && effect.type == VideoEffectType::Glow) {
                effect.param2 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::Glow) {
                effect.param3 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::FindEdges) {
                effect.param1 = value; return;
            }
            if (paramName == "angle" && effect.type == VideoEffectType::Emboss) {
                effect.param1 = value; return;
            }
            if (paramName == "amount" && effect.type == VideoEffectType::Emboss) {
                effect.param2 = value; return;
            }
            if (paramName == "levels" && effect.type == VideoEffectType::Posterize) {
                effect.param1 = value; return;
            }
            if (paramName == "level" && effect.type == VideoEffectType::Threshold) {
                effect.param1 = value; return;
            }
            if (paramName == "threshold" && effect.type == VideoEffectType::Solarize) {
                effect.param1 = value; return;
            }
            if (paramName == "inputBlack" && effect.type == VideoEffectType::Levels) {
                effect.param1 = value; return;
            }
            if (paramName == "inputWhite" && effect.type == VideoEffectType::Levels) {
                effect.param2 = value; return;
            }
            if (paramName == "gamma" && effect.type == VideoEffectType::Levels) {
                effect.param3 = value; return;
            }
            if (paramName == "amount" && effect.type == VideoEffectType::Tint) {
                effect.param1 = value; return;
            }
            if (paramName == "redWeight" && effect.type == VideoEffectType::BlackWhite) {
                effect.param1 = value; return;
            }
            if (paramName == "greenWeight" && effect.type == VideoEffectType::BlackWhite) {
                effect.param2 = value; return;
            }
            if (paramName == "blueWeight" && effect.type == VideoEffectType::BlackWhite) {
                effect.param3 = value; return;
            }
            if (paramName == "stops" && effect.type == VideoEffectType::Exposure) {
                effect.param1 = value; return;
            }
            if (paramName == "hueDegrees" && effect.type == VideoEffectType::HueSaturation) {
                effect.param1 = value; return;
            }
            if (paramName == "saturation" && effect.type == VideoEffectType::HueSaturation) {
                effect.param2 = value; return;
            }
            if (paramName == "lightness" && effect.type == VideoEffectType::HueSaturation) {
                effect.param3 = value; return;
            }
            if (paramName == "offsetX" && effect.type == VideoEffectType::RGBSplit) {
                effect.param1 = value; return;
            }
            if (paramName == "offsetY" && effect.type == VideoEffectType::RGBSplit) {
                effect.param2 = value; return;
            }
            if (paramName == "amplitude" && effect.type == VideoEffectType::WaveWarp) {
                effect.param1 = value; return;
            }
            if (paramName == "wavelength" && effect.type == VideoEffectType::WaveWarp) {
                effect.param2 = value; return;
            }
            if (paramName == "phase" && effect.type == VideoEffectType::WaveWarp) {
                effect.param3 = value; return;
            }
            if (paramName == "amplitude" && effect.type == VideoEffectType::Ripple) {
                effect.param1 = value; return;
            }
            if (paramName == "wavelength" && effect.type == VideoEffectType::Ripple) {
                effect.param2 = value; return;
            }
            if (paramName == "phase" && effect.type == VideoEffectType::Ripple) {
                effect.param3 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::GlitchVHS) {
                effect.param1 = value; return;
            }
            if (paramName == "blockHeight" && effect.type == VideoEffectType::GlitchVHS) {
                effect.param2 = value; return;
            }
            if (paramName == "seed" && effect.type == VideoEffectType::GlitchVHS) {
                effect.param3 = value; return;
            }
            if (paramName == "type" && effect.type == VideoEffectType::GradientRamp) {
                effect.param1 = value; return;
            }
            if (paramName == "angle" && effect.type == VideoEffectType::GradientRamp) {
                effect.param2 = value; return;
            }
            if (paramName == "opacity" && effect.type == VideoEffectType::GradientRamp) {
                effect.param3 = value; return;
            }
            if (paramName == "opacity" && effect.type == VideoEffectType::Fill) {
                effect.param1 = value; return;
            }
            if (paramName == "threshold" && effect.type == VideoEffectType::Bloom) {
                effect.param1 = value; return;
            }
            if (paramName == "radius" && effect.type == VideoEffectType::Bloom) {
                effect.param2 = value; return;
            }
            if (paramName == "intensity" && effect.type == VideoEffectType::Bloom) {
                effect.param3 = value; return;
            }
            if (paramName == "lineSpacing" && effect.type == VideoEffectType::Scanlines) {
                effect.param1 = value; return;
            }
            if (paramName == "darkness" && effect.type == VideoEffectType::Scanlines) {
                effect.param2 = value; return;
            }
            if (paramName == "opacity" && effect.type == VideoEffectType::Scanlines) {
                effect.param3 = value; return;
            }
            if (paramName == "dotSize" && effect.type == VideoEffectType::Halftone) {
                effect.param1 = value; return;
            }
            if (paramName == "angle" && effect.type == VideoEffectType::Halftone) {
                effect.param2 = value; return;
            }
            if (paramName == "shadows" && effect.type == VideoEffectType::Curves) {
                effect.param1 = value; return;
            }
            if (paramName == "highlights" && effect.type == VideoEffectType::Curves) {
                effect.param2 = value; return;
            }
            if (paramName == "midContrast" && effect.type == VideoEffectType::Curves) {
                effect.param3 = value; return;
            }
            if (paramName == "redFromRed" && effect.type == VideoEffectType::ChannelMixer) {
                effect.param1 = value; return;
            }
            if (paramName == "greenFromGreen" && effect.type == VideoEffectType::ChannelMixer) {
                effect.param2 = value; return;
            }
            if (paramName == "blueFromBlue" && effect.type == VideoEffectType::ChannelMixer) {
                effect.param3 = value; return;
            }
            if (paramName == "vibrance" && effect.type == VideoEffectType::Vibrance) {
                effect.param1 = value; return;
            }
            if (paramName == "density" && effect.type == VideoEffectType::PhotoFilter) {
                effect.param1 = value; return;
            }
            if (paramName == "blend" && effect.type == VideoEffectType::Tritone) {
                effect.param1 = value; return;
            }
            if (paramName == "brightness" && effect.type == VideoEffectType::BrightnessContrast) {
                effect.param1 = value; return;
            }
            if (paramName == "contrast" && effect.type == VideoEffectType::BrightnessContrast) {
                effect.param2 = value; return;
            }
            return;
        }
    }
}

QColor colorParamValue(const VideoEffect &effect, const QString &paramName)
{
    if (paramName == "color" && effect.type == VideoEffectType::ChromaKey)
        return effect.keyColor;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Tint)
        return effect.keyColor;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::GradientRamp)
        return effect.keyColor;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Fill)
        return effect.keyColor;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::PhotoFilter)
        return effect.keyColor;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Tritone)
        return effect.keyColor;
    return QColor();
}

void setColorParam(VideoEffect &effect, const QString &paramName, QColor color)
{
    if (paramName == "color" && effect.type == VideoEffectType::ChromaKey)
        effect.keyColor = color;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Tint)
        effect.keyColor = color;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::GradientRamp)
        effect.keyColor = color;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Fill)
        effect.keyColor = color;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::PhotoFilter)
        effect.keyColor = color;
    if ((paramName == "keyColor" || paramName == "color") && effect.type == VideoEffectType::Tritone)
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
    case VideoEffectType::GaussianBlur: return applyGaussianBlur(input, effect.param1);
    case VideoEffectType::DirectionalBlur: return applyDirectionalBlur(input, effect.param1, effect.param2);
    case VideoEffectType::RadialBlur: return applyRadialBlur(input, effect.param1, static_cast<int>(effect.param2));
    case VideoEffectType::Glow: return applyGlow(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::FindEdges: return applyFindEdges(input, effect.param1);
    case VideoEffectType::Emboss: return applyEmboss(input, effect.param1, effect.param2);
    case VideoEffectType::Posterize: return applyPosterize(input, effect.param1);
    case VideoEffectType::Threshold: return applyThreshold(input, effect.param1);
    case VideoEffectType::Solarize: return applySolarize(input, effect.param1);
    case VideoEffectType::Levels: return applyLevels(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Tint: return applyTint(input, effect.param1, effect.keyColor);
    case VideoEffectType::BlackWhite: return applyBlackWhite(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Exposure: return applyExposureEffect(input, effect.param1);
    case VideoEffectType::HueSaturation: return applyHueSaturation(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::RGBSplit: return applyRGBSplit(input, effect.param1, effect.param2);
    case VideoEffectType::WaveWarp: return applyWaveWarp(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Ripple: return applyRipple(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::GlitchVHS: return applyGlitchVHS(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::GradientRamp: return applyGradientRamp(input, static_cast<int>(effect.param1), effect.param2, effect.param3, effect.keyColor);
    case VideoEffectType::Fill: return applyFill(input, effect.keyColor, effect.param1);
    case VideoEffectType::Bloom: return applyBloom(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Scanlines: return applyScanlines(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Halftone: return applyHalftone(input, effect.param1, effect.param2);
    case VideoEffectType::Curves: return applyCurves(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::ChannelMixer: return applyChannelMixer(input, effect.param1, effect.param2, effect.param3);
    case VideoEffectType::Vibrance: return applyVibrance(input, effect.param1);
    case VideoEffectType::PhotoFilter: return applyPhotoFilter(input, effect.keyColor, effect.param1);
    case VideoEffectType::Tritone: return applyTritone(input, effect.keyColor, effect.param1);
    case VideoEffectType::BrightnessContrast: return applyBrightnessContrastEffect(input, effect.param1, effect.param2);
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

QImage VideoEffectProcessor::applyGaussianBlur(const QImage &input, double radius)
{
    const double sigma = qBound(0.0, radius, 50.0);
    if (sigma <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_RGB888);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_RGB888);
    QVector<double> tmp(w * h * 3, 0.0);

    const int kernelRadius = qMax(1, static_cast<int>(std::ceil(sigma * 3.0)));
    QVector<double> kernel(kernelRadius * 2 + 1);
    double kernelSum = 0.0;
    const double denom = 2.0 * sigma * sigma;
    for (int i = -kernelRadius; i <= kernelRadius; ++i) {
        const double weight = std::exp(-(i * i) / denom);
        kernel[i + kernelRadius] = weight;
        kernelSum += weight;
    }
    for (double &weight : kernel)
        weight /= kernelSum;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double rSum = 0.0, gSum = 0.0, bSum = 0.0;
            for (int k = -kernelRadius; k <= kernelRadius; ++k) {
                const int sx = qBound(0, x + k, w - 1);
                const uint8_t *srcLine = src.constScanLine(y);
                const double weight = kernel[k + kernelRadius];
                rSum += srcLine[sx * 3] * weight;
                gSum += srcLine[sx * 3 + 1] * weight;
                bSum += srcLine[sx * 3 + 2] * weight;
            }
            const int idx = (y * w + x) * 3;
            tmp[idx] = rSum;
            tmp[idx + 1] = gSum;
            tmp[idx + 2] = bSum;
        }
    }

    double rErr = 0.0;
    double gErr = 0.0;
    double bErr = 0.0;
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double rSum = 0.0, gSum = 0.0, bSum = 0.0;
            for (int k = -kernelRadius; k <= kernelRadius; ++k) {
                const int sy = qBound(0, y + k, h - 1);
                const int srcIdx = (sy * w + x) * 3;
                const double weight = kernel[k + kernelRadius];
                rSum += tmp[srcIdx] * weight;
                gSum += tmp[srcIdx + 1] * weight;
                bSum += tmp[srcIdx + 2] * weight;
            }
            const double rQuant = rSum + rErr;
            const double gQuant = gSum + gErr;
            const double bQuant = bSum + bErr;
            dst[x * 3] = clamp255d(rQuant);
            dst[x * 3 + 1] = clamp255d(gQuant);
            dst[x * 3 + 2] = clamp255d(bQuant);
            rErr = rQuant - dst[x * 3];
            gErr = gQuant - dst[x * 3 + 1];
            bErr = bQuant - dst[x * 3 + 2];
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyDirectionalBlur(const QImage &input, double angleDegrees, double lengthPx)
{
    const int steps = qBound(0, static_cast<int>(std::round(lengthPx)), 100);
    if (steps <= 0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_RGB888);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_RGB888);
    const double angle = angleDegrees * M_PI / 180.0;
    const double dx = std::cos(angle);
    const double dy = std::sin(angle);
    const double sampleCount = static_cast<double>(steps * 2 + 1);

    for (int y = 0; y < h; ++y) {
        uint8_t *dst = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double rSum = 0.0, gSum = 0.0, bSum = 0.0;
            for (int i = -steps; i <= steps; ++i) {
                const int sx = static_cast<int>(std::round(x + dx * i));
                const int sy = static_cast<int>(std::round(y + dy * i));
                addRgbSample(src, sx, sy, rSum, gSum, bSum);
            }
            dst[x * 3] = clamp255d(rSum / sampleCount);
            dst[x * 3 + 1] = clamp255d(gSum / sampleCount);
            dst[x * 3 + 2] = clamp255d(bSum / sampleCount);
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyRadialBlur(const QImage &input, double amount, int mode)
{
    const int steps = qBound(0, static_cast<int>(std::round(amount)), 50);
    if (steps <= 0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_RGB888);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_RGB888);
    const double cx = w * 0.5;
    const double cy = h * 0.5;

    if (mode == 1) {
        const double sampleCount = static_cast<double>(steps + 1);
        for (int y = 0; y < h; ++y) {
            uint8_t *dst = result.scanLine(y);
            for (int x = 0; x < w; ++x) {
                double rSum = 0.0, gSum = 0.0, bSum = 0.0;
                for (int i = 0; i <= steps; ++i) {
                    const double t = static_cast<double>(i) / steps;
                    const int sx = static_cast<int>(std::round(cx + (x - cx) * t));
                    const int sy = static_cast<int>(std::round(cy + (y - cy) * t));
                    addRgbSample(src, sx, sy, rSum, gSum, bSum);
                }
                dst[x * 3] = clamp255d(rSum / sampleCount);
                dst[x * 3 + 1] = clamp255d(gSum / sampleCount);
                dst[x * 3 + 2] = clamp255d(bSum / sampleCount);
            }
        }
        return result;
    }

    const double arc = amount * M_PI / 180.0;
    const double sampleCount = static_cast<double>(steps * 2 + 1);
    for (int y = 0; y < h; ++y) {
        uint8_t *dst = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const double vx = x - cx;
            const double vy = y - cy;
            const double radius = std::sqrt(vx * vx + vy * vy);
            const double baseAngle = std::atan2(vy, vx);
            double rSum = 0.0, gSum = 0.0, bSum = 0.0;
            for (int i = -steps; i <= steps; ++i) {
                const double a = baseAngle + arc * static_cast<double>(i) / steps;
                const int sx = static_cast<int>(std::round(cx + std::cos(a) * radius));
                const int sy = static_cast<int>(std::round(cy + std::sin(a) * radius));
                addRgbSample(src, sx, sy, rSum, gSum, bSum);
            }
            dst[x * 3] = clamp255d(rSum / sampleCount);
            dst[x * 3 + 1] = clamp255d(gSum / sampleCount);
            dst[x * 3 + 2] = clamp255d(bSum / sampleCount);
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyGlow(const QImage &input, double threshold, double radius, double intensity)
{
    const double glowIntensity = qBound(0.0, intensity, 3.0);
    if (glowIntensity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    QVector<double> bright(w * h * 3, 0.0);
    const double t = qBound(0.0, threshold, 255.0);

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int r = qRed(line[x]);
            const int g = qGreen(line[x]);
            const int b = qBlue(line[x]);
            if (luma709(r, g, b) > t) {
                const int idx = (y * w + x) * 3;
                bright[idx] = r;
                bright[idx + 1] = g;
                bright[idx + 2] = b;
            }
        }
    }

    const int blurRadius = qBound(0, static_cast<int>(std::round(radius)), 50);
    const QVector<double> glow = boxBlurRgbBuffer(bright, w, h, blurRadius);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int idx = (y * w + x) * 3;
            const int r = clamp255d(qRed(srcLine[x]) + glow[idx] * glowIntensity);
            const int g = clamp255d(qGreen(srcLine[x]) + glow[idx + 1] * glowIntensity);
            const int b = clamp255d(qBlue(srcLine[x]) + glow[idx + 2] * glowIntensity);
            dstLine[x] = qRgba(r, g, b, qAlpha(srcLine[x]));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyFindEdges(const QImage &input, double intensity)
{
    const double edgeIntensity = qBound(0.0, intensity, 2.0);
    if (edgeIntensity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    auto sampleLuma = [&](int x, int y) -> double {
        const int sx = qBound(0, x, w - 1);
        const int sy = qBound(0, y, h - 1);
        const QRgb *line = reinterpret_cast<const QRgb*>(src.constScanLine(sy));
        return luma709(qRed(line[sx]), qGreen(line[sx]), qBlue(line[sx]));
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double gx =
                -sampleLuma(x - 1, y - 1) + sampleLuma(x + 1, y - 1)
                -2.0 * sampleLuma(x - 1, y) + 2.0 * sampleLuma(x + 1, y)
                -sampleLuma(x - 1, y + 1) + sampleLuma(x + 1, y + 1);
            const double gy =
                -sampleLuma(x - 1, y - 1) - 2.0 * sampleLuma(x, y - 1) - sampleLuma(x + 1, y - 1)
                + sampleLuma(x - 1, y + 1) + 2.0 * sampleLuma(x, y + 1) + sampleLuma(x + 1, y + 1);
            const int edge = clamp255d(std::sqrt(gx * gx + gy * gy) * edgeIntensity);
            dstLine[x] = qRgba(edge, edge, edge, qAlpha(srcLine[x]));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyEmboss(const QImage &input, double angleDegrees, double amount)
{
    const double embossAmount = qBound(0.0, amount, 5.0);
    if (embossAmount <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double angle = angleDegrees * M_PI / 180.0;
    int dx = static_cast<int>(std::round(std::cos(angle)));
    int dy = static_cast<int>(std::round(std::sin(angle)));
    if (dx == 0 && dy == 0)
        dx = 1;

    auto sampleLuma = [&](int x, int y) -> double {
        const int sx = qBound(0, x, w - 1);
        const int sy = qBound(0, y, h - 1);
        const QRgb *line = reinterpret_cast<const QRgb*>(src.constScanLine(sy));
        return luma709(qRed(line[sx]), qGreen(line[sx]), qBlue(line[sx]));
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double hi = sampleLuma(x + dx, y + dy);
            const double lo = sampleLuma(x - dx, y - dy);
            const int relief = clamp255d(128.0 + (hi - lo) * embossAmount);
            dstLine[x] = qRgba(relief, relief, relief, qAlpha(srcLine[x]));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyPosterize(const QImage &input, double levels)
{
    const int levelCount = qBound(2, static_cast<int>(std::round(levels)), 256);
    if (levelCount >= 256)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double bucketSize = 256.0 / levelCount;
    const double outScale = 255.0 / (levelCount - 1);

    auto quantize = [&](int value) -> int {
        const int bucket = qMin(levelCount - 1, static_cast<int>(std::floor(value / bucketSize)));
        return clamp255d(bucket * outScale);
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            dstLine[x] = qRgba(quantize(qRed(srcLine[x])),
                               quantize(qGreen(srcLine[x])),
                               quantize(qBlue(srcLine[x])),
                               qAlpha(srcLine[x]));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyThreshold(const QImage &input, double level)
{
    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double threshold = qBound(0.0, level, 255.0);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int value = luma709(qRed(px), qGreen(px), qBlue(px)) >= threshold ? 255 : 0;
            dstLine[x] = qRgba(value, value, value, qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applySolarize(const QImage &input, double threshold)
{
    const double t = qBound(0.0, threshold, 255.0);
    if (t >= 255.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            if (luma709(qRed(px), qGreen(px), qBlue(px)) > t) {
                dstLine[x] = qRgba(255 - qRed(px),
                                   255 - qGreen(px),
                                   255 - qBlue(px),
                                   qAlpha(px));
            } else {
                dstLine[x] = px;
            }
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyLevels(const QImage &input, double inputBlack, double inputWhite, double gamma)
{
    if (inputBlack <= 0.0 && inputWhite >= 255.0 && gamma == 1.0)
        return input;

    int black = qBound(0, static_cast<int>(std::round(inputBlack)), 255);
    int white = qBound(0, static_cast<int>(std::round(inputWhite)), 255);
    if (white <= black) {
        if (black >= 255)
            black = 254;
        white = black + 1;
    }
    const double g = qBound(0.1, gamma, 5.0);
    const double scale = 1.0 / (white - black);

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    auto remap = [&](int value) -> int {
        if (value <= black)
            return 0;
        if (value >= white)
            return 255;
        const double normalized = (value - black) * scale;
        return clamp255d(std::pow(normalized, g) * 255.0);
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(remap(qRed(px)),
                               remap(qGreen(px)),
                               remap(qBlue(px)),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyTint(const QImage &input, double amount, QColor highlightTint)
{
    const double tintAmount = qBound(0.0, amount, 1.0);
    if (tintAmount <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const int kr = highlightTint.red();
    const int kg = highlightTint.green();
    const int kb = highlightTint.blue();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const double gray = luma709(qRed(px), qGreen(px), qBlue(px)) / 255.0;
            const double tr = kr * gray;
            const double tg = kg * gray;
            const double tb = kb * gray;
            dstLine[x] = qRgba(clamp255d(qRed(px) + tintAmount * (tr - qRed(px))),
                               clamp255d(qGreen(px) + tintAmount * (tg - qGreen(px))),
                               clamp255d(qBlue(px) + tintAmount * (tb - qBlue(px))),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyBlackWhite(const QImage &input, double redWeight,
                                             double greenWeight, double blueWeight)
{
    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int gray = clamp255d(qRed(px) * redWeight
                                     + qGreen(px) * greenWeight
                                     + qBlue(px) * blueWeight);
            dstLine[x] = qRgba(gray, gray, gray, qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyExposureEffect(const QImage &input, double stops)
{
    if (stops == 0.0)
        return input;

    const double gain = std::pow(2.0, qBound(-5.0, stops, 5.0));
    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(clamp255d(qRed(px) * gain),
                               clamp255d(qGreen(px) * gain),
                               clamp255d(qBlue(px) * gain),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyHueSaturation(const QImage &input, double hueDegrees,
                                                double saturation, double lightness)
{
    if (hueDegrees == 0.0 && saturation == 0.0 && lightness == 0.0)
        return input;

    const double hueShift = qBound(-180.0, hueDegrees, 180.0) / 360.0;
    const double satFactor = (qBound(-100.0, saturation, 100.0) + 100.0) / 100.0;
    const double lightShift = qBound(-100.0, lightness, 100.0) / 100.0;
    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            QColor color(qRed(px), qGreen(px), qBlue(px), qAlpha(px));
            float hslH = 0.0f;
            float hslS = 0.0f;
            float hslL = 0.0f;
            float hslA = 1.0f;
            color.getHslF(&hslH, &hslS, &hslL, &hslA);
            if (hslH < 0.0f)
                hslH = 0.0f;
            hslH = static_cast<float>(std::fmod(static_cast<double>(hslH) + hueShift, 1.0));
            if (hslH < 0.0f)
                hslH += 1.0f;
            hslS = static_cast<float>(qBound(0.0, static_cast<double>(hslS) * satFactor, 1.0));
            hslL = static_cast<float>(qBound(0.0, static_cast<double>(hslL) + lightShift, 1.0));
            const QColor out = QColor::fromHslF(hslH, hslS, hslL, hslA);
            dstLine[x] = qRgba(out.red(), out.green(), out.blue(), qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyRGBSplit(const QImage &input, double offsetX, double offsetY)
{
    const int dx = qBound(-50, static_cast<int>(std::round(offsetX)), 50);
    const int dy = qBound(-50, static_cast<int>(std::round(offsetY)), 50);
    if (dx == 0 && dy == 0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb rPx = sampleArgbPixel(src, x + dx, y + dy);
            const QRgb gPx = sampleArgbPixel(src, x, y);
            const QRgb bPx = sampleArgbPixel(src, x - dx, y - dy);
            dstLine[x] = qRgba(qRed(rPx), qGreen(gPx), qBlue(bPx), qAlpha(gPx));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyWaveWarp(const QImage &input, double amplitude,
                                           double wavelength, double phaseDegrees)
{
    const double amp = qBound(0.0, amplitude, 100.0);
    if (amp <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double wave = qBound(1.0, wavelength, 500.0);
    const double phase = qBound(0.0, phaseDegrees, 360.0) * M_PI / 180.0;
    const double frequency = 2.0 * M_PI / wave;

    for (int y = 0; y < h; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double hOffset = std::sin(y * frequency + phase) * amp;
            const double vOffset = std::sin(x * frequency + phase) * amp;
            const int sx = static_cast<int>(std::round(x + hOffset));
            const int sy = static_cast<int>(std::round(y + vOffset));
            dstLine[x] = sampleArgbPixel(src, sx, sy);
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyRipple(const QImage &input, double amplitude,
                                         double wavelength, double phaseDegrees)
{
    const double amp = qBound(0.0, amplitude, 100.0);
    if (amp <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double wave = qBound(1.0, wavelength, 500.0);
    const double phase = qBound(0.0, phaseDegrees, 360.0) * M_PI / 180.0;
    const double frequency = 2.0 * M_PI / wave;
    const double cx = (w - 1) * 0.5;
    const double cy = (h - 1) * 0.5;

    for (int y = 0; y < h; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double vx = x - cx;
            const double vy = y - cy;
            const double dist = std::sqrt(vx * vx + vy * vy);
            if (dist <= 1e-9) {
                dstLine[x] = sampleArgbPixel(src, x, y);
                continue;
            }

            const double shift = std::sin(dist * frequency + phase) * amp;
            const int sx = static_cast<int>(std::round(x + (vx / dist) * shift));
            const int sy = static_cast<int>(std::round(y + (vy / dist) * shift));
            dstLine[x] = sampleArgbPixel(src, sx, sy);
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyGlitchVHS(const QImage &input, double intensity,
                                            double blockHeight, double seed)
{
    const double amount = qBound(0.0, intensity, 1.0);
    if (amount <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const int block = qBound(4, static_cast<int>(std::round(blockHeight)), 64);
    const int seedValue = qBound(0, static_cast<int>(std::round(seed)), 1000);
    const int maxShift = qMax(1, static_cast<int>(std::round(w * 0.25 * amount)));
    const int channelShift = qMax(1, static_cast<int>(std::round(3.0 * amount)));

    for (int y = 0; y < h; ++y) {
        const int blockIndex = y / block;
        const std::uint32_t hash = glitchHash(blockIndex, seedValue);
        const int shiftRange = maxShift * 2 + 1;
        const int blockShift = static_cast<int>(hash % static_cast<std::uint32_t>(shiftRange)) - maxShift;
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const int sx = x + blockShift;
            const QRgb center = sampleArgbPixel(src, sx, y);
            const QRgb rPx = sampleArgbPixel(src, sx + channelShift, y);
            const QRgb bPx = sampleArgbPixel(src, sx - channelShift, y);
            dstLine[x] = qRgba(qRed(rPx), qGreen(center), qBlue(bPx), qAlpha(center));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyGradientRamp(const QImage &input, int type,
                                               double angleDegrees, double opacity,
                                               QColor startColor)
{
    const double rampOpacity = qBound(0.0, opacity, 1.0);
    if (rampOpacity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const int kr = startColor.red();
    const int kg = startColor.green();
    const int kb = startColor.blue();

    if (type == 1) {
        const double cx = (w - 1) * 0.5;
        const double cy = (h - 1) * 0.5;
        const double maxDist = qMax(1e-9, std::sqrt(cx * cx + cy * cy));
        for (int y = 0; y < h; ++y) {
            const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
            QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb px = srcLine[x];
                const double dist = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                const double blend = rampOpacity * qBound(0.0, 1.0 - dist / maxDist, 1.0);
                dstLine[x] = qRgba(clamp255d(qRed(px) + blend * (kr - qRed(px))),
                                   clamp255d(qGreen(px) + blend * (kg - qGreen(px))),
                                   clamp255d(qBlue(px) + blend * (kb - qBlue(px))),
                                   qAlpha(px));
            }
        }
        return result;
    }

    const double angle = angleDegrees * M_PI / 180.0;
    const double dx = std::cos(angle);
    const double dy = std::sin(angle);
    const double corners[4] = {
        0.0,
        (w - 1) * dx,
        (h - 1) * dy,
        (w - 1) * dx + (h - 1) * dy
    };
    const double minProj = *std::min_element(corners, corners + 4);
    const double maxProj = *std::max_element(corners, corners + 4);
    const double denom = qMax(1e-9, maxProj - minProj);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const double t = qBound(0.0, (x * dx + y * dy - minProj) / denom, 1.0);
            const double blend = rampOpacity * t;
            dstLine[x] = qRgba(clamp255d(qRed(px) + blend * (kr - qRed(px))),
                               clamp255d(qGreen(px) + blend * (kg - qGreen(px))),
                               clamp255d(qBlue(px) + blend * (kb - qBlue(px))),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyFill(const QImage &input, QColor fillColor, double opacity)
{
    const double fillOpacity = qBound(0.0, opacity, 1.0);
    if (fillOpacity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const int kr = fillColor.red();
    const int kg = fillColor.green();
    const int kb = fillColor.blue();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(clamp255d(qRed(px) + fillOpacity * (kr - qRed(px))),
                               clamp255d(qGreen(px) + fillOpacity * (kg - qGreen(px))),
                               clamp255d(qBlue(px) + fillOpacity * (kb - qBlue(px))),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyBloom(const QImage &input, double threshold,
                                        double radius, double intensity)
{
    const double bloomIntensity = qBound(0.0, intensity, 2.0);
    if (bloomIntensity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    QVector<double> bright(w * h * 3, 0.0);
    const double t = qBound(0.0, threshold, 255.0);

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int r = qRed(line[x]);
            const int g = qGreen(line[x]);
            const int b = qBlue(line[x]);
            if (luma709(r, g, b) > t) {
                const int idx = (y * w + x) * 3;
                bright[idx] = r;
                bright[idx + 1] = g;
                bright[idx + 2] = b;
            }
        }
    }

    const int blurRadius = qBound(0, static_cast<int>(std::round(radius)), 80);
    QVector<double> bloom = bright;
    for (int pass = 0; pass < 3; ++pass)
        bloom = boxBlurRgbBuffer(bloom, w, h, blurRadius);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int idx = (y * w + x) * 3;
            const double br = qBound(0.0, bloom[idx] * bloomIntensity, 255.0);
            const double bg = qBound(0.0, bloom[idx + 1] * bloomIntensity, 255.0);
            const double bb = qBound(0.0, bloom[idx + 2] * bloomIntensity, 255.0);
            dstLine[x] = qRgba(clamp255d(255.0 - (255.0 - qRed(px)) * (255.0 - br) / 255.0),
                               clamp255d(255.0 - (255.0 - qGreen(px)) * (255.0 - bg) / 255.0),
                               clamp255d(255.0 - (255.0 - qBlue(px)) * (255.0 - bb) / 255.0),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyScanlines(const QImage &input, double lineSpacing,
                                            double darkness, double opacity)
{
    const double dark = qBound(0.0, darkness, 1.0);
    const double scanOpacity = qBound(0.0, opacity, 1.0);
    if (dark <= 0.0 || scanOpacity <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const int spacing = qBound(2, static_cast<int>(std::round(lineSpacing)), 20);
    const double amount = dark * scanOpacity;
    const double maskAmount = amount * 0.08;

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        const double scanMul = (y % spacing) == 0 ? (1.0 - amount) : 1.0;
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            double rm = scanMul;
            double gm = scanMul;
            double bm = scanMul;
            switch (x % 3) {
            case 0:
                rm *= 1.0 + maskAmount;
                gm *= 1.0 - maskAmount;
                bm *= 1.0 - maskAmount;
                break;
            case 1:
                rm *= 1.0 - maskAmount;
                gm *= 1.0 + maskAmount;
                bm *= 1.0 - maskAmount;
                break;
            default:
                rm *= 1.0 - maskAmount;
                gm *= 1.0 - maskAmount;
                bm *= 1.0 + maskAmount;
                break;
            }
            dstLine[x] = qRgba(clamp255d(qRed(px) * rm),
                               clamp255d(qGreen(px) * gm),
                               clamp255d(qBlue(px) * bm),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyHalftone(const QImage &input, double dotSize,
                                           double angleDegrees)
{
    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double size = static_cast<double>(qBound(2, static_cast<int>(std::round(dotSize)), 30));
    const double angle = angleDegrees * M_PI / 180.0;
    const double ca = std::cos(angle);
    const double sa = std::sin(angle);
    const double cx = (w - 1) * 0.5;
    const double cy = (h - 1) * 0.5;
    const double maxRadius = size * 0.7071067811865476;

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const double tx = x - cx;
            const double ty = y - cy;
            const double rx = tx * ca + ty * sa;
            const double ry = -tx * sa + ty * ca;
            const double cellCx = (std::floor(rx / size) + 0.5) * size;
            const double cellCy = (std::floor(ry / size) + 0.5) * size;
            const double dist = std::sqrt((rx - cellCx) * (rx - cellCx)
                                        + (ry - cellCy) * (ry - cellCy));
            const double darkness = 1.0 - luma709(qRed(px), qGreen(px), qBlue(px)) / 255.0;
            const double radius = std::sqrt(qBound(0.0, darkness, 1.0)) * maxRadius;
            const int value = dist <= radius ? 0 : 255;
            dstLine[x] = qRgba(value, value, value, qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyCurves(const QImage &input, double shadows,
                                         double highlights, double midContrast)
{
    const double shadowAdjust = qBound(-100.0, shadows, 100.0) / 100.0;
    const double highlightAdjust = qBound(-100.0, highlights, 100.0) / 100.0;
    const double midAdjust = qBound(-100.0, midContrast, 100.0) / 100.0;
    if (shadowAdjust == 0.0 && highlightAdjust == 0.0 && midAdjust == 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double contrastFactor = std::pow(2.0, midAdjust);

    auto curve = [&](int value) -> int {
        const double v = value / 255.0;
        double out = v;
        const double shadowWeight = 1.0 - smoothstep(v);
        const double highlightWeight = smoothstep(v);
        out += shadowAdjust * 0.35 * shadowWeight * (1.0 - v);
        out += highlightAdjust * 0.35 * highlightWeight * v;
        out = 0.5 + (out - 0.5) * contrastFactor;
        return clamp255d(qBound(0.0, out, 1.0) * 255.0);
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(curve(qRed(px)),
                               curve(qGreen(px)),
                               curve(qBlue(px)),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyChannelMixer(const QImage &input, double redFromRed,
                                               double greenFromGreen, double blueFromBlue)
{
    const double redGain = qBound(0.0, redFromRed, 200.0) / 100.0;
    const double greenGain = qBound(0.0, greenFromGreen, 200.0) / 100.0;
    const double blueGain = qBound(0.0, blueFromBlue, 200.0) / 100.0;
    if (redGain == 1.0 && greenGain == 1.0 && blueGain == 1.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(clamp255d(qRed(px) * redGain),
                               clamp255d(qGreen(px) * greenGain),
                               clamp255d(qBlue(px) * blueGain),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyVibrance(const QImage &input, double vibrance)
{
    const double amount = qBound(-100.0, vibrance, 100.0) / 100.0;
    if (amount == 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    auto hueDegrees = [](int r, int g, int b) -> double {
        const double rd = r / 255.0;
        const double gd = g / 255.0;
        const double bd = b / 255.0;
        const double maxC = std::max({ rd, gd, bd });
        const double minC = std::min({ rd, gd, bd });
        const double chroma = maxC - minC;
        if (chroma <= 1e-9)
            return 0.0;
        double hue = 0.0;
        if (maxC == rd) {
            hue = 60.0 * std::fmod((gd - bd) / chroma, 6.0);
        } else if (maxC == gd) {
            hue = 60.0 * (((bd - rd) / chroma) + 2.0);
        } else {
            hue = 60.0 * (((rd - gd) / chroma) + 4.0);
        }
        if (hue < 0.0)
            hue += 360.0;
        return hue;
    };

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int r = qRed(px);
            const int g = qGreen(px);
            const int b = qBlue(px);
            const double saturation = rgbSaturation01(r, g, b);
            const double hue = hueDegrees(r, g, b);
            const bool skinHue = hue >= 15.0 && hue <= 55.0 && r > g && g >= b
                && saturation > 0.08 && saturation < 0.75;
            const double skinProtect = skinHue ? 0.45 : 1.0;
            const double boost = amount > 0.0
                ? amount * (1.0 - saturation) * 1.25 * skinProtect
                : amount * (0.6 + 0.4 * (1.0 - saturation));
            const double factor = qMax(0.0, 1.0 + boost);
            const double lum = luma709(r, g, b);
            dstLine[x] = qRgba(clamp255d(lum + (r - lum) * factor),
                               clamp255d(lum + (g - lum) * factor),
                               clamp255d(lum + (b - lum) * factor),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyPhotoFilter(const QImage &input, QColor filterColor,
                                              double density)
{
    const double amount = qBound(0.0, density, 100.0) / 100.0;
    if (amount <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double fr = filterColor.red();
    const double fg = filterColor.green();
    const double fb = filterColor.blue();
    const double filterLum = luma709(filterColor.red(), filterColor.green(), filterColor.blue());

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const double lum = luma709(qRed(px), qGreen(px), qBlue(px));
            double tr = lum;
            double tg = lum;
            double tb = lum;
            if (filterLum > 1e-9) {
                const double scale = lum / filterLum;
                tr = qBound(0.0, fr * scale, 255.0);
                tg = qBound(0.0, fg * scale, 255.0);
                tb = qBound(0.0, fb * scale, 255.0);
            }
            double outR = qRed(px) + amount * (tr - qRed(px));
            double outG = qGreen(px) + amount * (tg - qGreen(px));
            double outB = qBlue(px) + amount * (tb - qBlue(px));
            const double lumaDelta = lum - luma709(clamp255d(outR), clamp255d(outG), clamp255d(outB));
            outR += lumaDelta;
            outG += lumaDelta;
            outB += lumaDelta;
            dstLine[x] = qRgba(clamp255d(outR),
                               clamp255d(outG),
                               clamp255d(outB),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyTritone(const QImage &input, QColor shadowColor,
                                          double blend)
{
    const double amount = qBound(0.0, blend, 1.0);
    if (amount <= 0.0)
        return input;

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);
    const double sr = shadowColor.red();
    const double sg = shadowColor.green();
    const double sb = shadowColor.blue();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const double lum = luma709(qRed(px), qGreen(px), qBlue(px)) / 255.0;
            double tr = 128.0;
            double tg = 128.0;
            double tb = 128.0;
            if (lum <= 0.5) {
                const double t = smoothstep(lum / 0.5);
                tr = sr + t * (128.0 - sr);
                tg = sg + t * (128.0 - sg);
                tb = sb + t * (128.0 - sb);
            } else {
                const double t = smoothstep((lum - 0.5) / 0.5);
                tr = 128.0 + t * 127.0;
                tg = 128.0 + t * 127.0;
                tb = 128.0 + t * 127.0;
            }
            dstLine[x] = qRgba(clamp255d(qRed(px) + amount * (tr - qRed(px))),
                               clamp255d(qGreen(px) + amount * (tg - qGreen(px))),
                               clamp255d(qBlue(px) + amount * (tb - qBlue(px))),
                               qAlpha(px));
        }
    }

    return result;
}

QImage VideoEffectProcessor::applyBrightnessContrastEffect(const QImage &input,
                                                           double brightness,
                                                           double contrast)
{
    const double b = qBound(-100.0, brightness, 100.0);
    const double c = qBound(-100.0, contrast, 100.0);
    if (b == 0.0 && c == 0.0)
        return input;

    const double brightnessOffset = b * 2.55;
    double contrastFactor = (100.0 + c) / 100.0;
    contrastFactor *= contrastFactor;

    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        double v = i + brightnessOffset;
        v = ((v / 255.0 - 0.5) * contrastFactor + 0.5) * 255.0;
        lut[i] = static_cast<uint8_t>(clamp255d(v));
    }

    QImage src = input.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            dstLine[x] = qRgba(lut[qRed(px)],
                               lut[qGreen(px)],
                               lut[qBlue(px)],
                               qAlpha(px));
        }
    }

    return result;
}
