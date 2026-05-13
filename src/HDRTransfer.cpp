#include "HDRTransfer.h"

#include <cmath>
#include <algorithm>
#include <QColor>

namespace hdr {

// ---------------------------------------------------------------------------
// PQ (ST.2084) constants
// ---------------------------------------------------------------------------
static constexpr float kPqM1 = 0.1593017578125f;
static constexpr float kPqM2 = 78.84375f;
static constexpr float kPqC1 = 0.8359375f;
static constexpr float kPqC2 = 18.8515625f;
static constexpr float kPqC3 = 18.6875f;

// PQ EOTF: normalized [0..1] -> linear scene-referred nits [0..10000]
// Returns value in [0..1] range (normalized by 10000 nits peak)
float pqEotf(float v)
{
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;

    const double vd = static_cast<double>(v);
    const double vPow = std::pow(vd, 1.0 / kPqM2);
    const double num  = std::max(vPow - kPqC1, 0.0);
    const double den  = kPqC2 - kPqC3 * vPow;
    if (den <= 0.0) return 1.0f;
    const double linear_nits = std::pow(num / den, 1.0 / kPqM1) * 10000.0;
    const float result = static_cast<float>(linear_nits / 10000.0);
    if (std::isnan(result) || std::isinf(result)) return 0.0f;
    return std::max(0.0f, std::min(1.0f, result));
}

// PQ OETF: linear [0..1] (normalized, peak = 10000 nits) -> normalized [0..1]
float pqOetf(float L)
{
    if (L <= 0.0f) return 0.0f;
    if (L >= 1.0f) return 1.0f;

    const double Ld = static_cast<double>(L);
    const double Ym1 = std::pow(Ld, kPqM1);
    const double num = kPqC1 + kPqC2 * Ym1;
    const double den = 1.0 + kPqC3 * Ym1;
    const float result = static_cast<float>(std::pow(num / den, kPqM2));
    if (std::isnan(result) || std::isinf(result)) return 0.0f;
    return std::max(0.0f, std::min(1.0f, result));
}

// ---------------------------------------------------------------------------
// HLG (BT.2100 / ARIB STD-B67) constants
// ---------------------------------------------------------------------------
static constexpr double kHlgA = 0.17883277;
static constexpr double kHlgB = 0.28466892;  // 1 - 4*a
static constexpr double kHlgC = 0.55991073;  // 0.5 - a*ln(4*a)

// HLG OETF: linear [0..1] -> normalized [0..1]
// Linear is normalized so that peak display luminance = 12 cd/m2 reference
float hlgOetf(float L)
{
    if (std::isnan(L) || std::isinf(L)) return 0.0f;
    if (L <= 0.0f) return 0.0f;
    if (L >= 1.0f) return 1.0f;  // upper bound guard — clamps scene-linear >= 1.0

    const double Ld = static_cast<double>(L);
    double result;
    if (Ld <= 1.0 / 12.0) {
        result = std::sqrt(3.0 * Ld);
    } else {
        result = kHlgA * std::log(12.0 * Ld - kHlgB) + kHlgC;
    }
    if (std::isnan(result) || std::isinf(result)) return 0.0f;
    return static_cast<float>(std::max(0.0, std::min(1.0, result)));
}

// HLG EOTF: normalized [0..1] -> linear [0..1]
float hlgEotf(float v)
{
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;

    const double vd = static_cast<double>(v);
    double result;
    if (vd <= 0.5) {
        result = (vd * vd) / 3.0;
    } else {
        result = (std::exp((vd - kHlgC) / kHlgA) + kHlgB) / 12.0;
    }
    if (std::isnan(result) || std::isinf(result)) return 0.0f;
    return static_cast<float>(std::max(0.0, std::min(1.0, result)));
}

// ---------------------------------------------------------------------------
// Helper: clamp to [0..255]
// ---------------------------------------------------------------------------
static int clamp8(double v)
{
    if (std::isnan(v) || std::isinf(v)) return 0;
    const int i = static_cast<int>(v + 0.5);
    return std::max(0, std::min(255, i));
}

// ---------------------------------------------------------------------------
// Hable filmic curve helper
// ---------------------------------------------------------------------------
static constexpr double kHableA = 0.15;
static constexpr double kHableB = 0.50;
static constexpr double kHableC = 0.10;
static constexpr double kHableD = 0.20;
static constexpr double kHableE = 0.02;
static constexpr double kHableF = 0.30;

static double hableCurve(double x)
{
    const double denom = x * (kHableA * x + kHableB) + kHableD * kHableF;
    if (denom == 0.0) return 0.0;
    return ((x * (kHableA * x + kHableC * kHableB) + kHableD * kHableE) / denom) - kHableE / kHableF;
}

// ---------------------------------------------------------------------------
// applyToneMapReinhard
// ---------------------------------------------------------------------------
// Luminance-based (hue-preserving) Reinhard tone map.
// Computes Rec.2020 luma L = 0.2627R + 0.6780G + 0.0593B,
// then applies single scale = 1 / (1 + L) to all channels.
QImage applyToneMapReinhard(const QImage& linearRec2020, double exposure)
{
    const QImage src = linearRec2020.convertToFormat(QImage::Format_ARGB32);
    QImage dst(src.size(), QImage::Format_ARGB32);

    const int w = src.width();
    const int h = src.height();

    for (int y = 0; y < h; ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb*       dstLine = reinterpret_cast<QRgb*>(dst.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int  a  = qAlpha(px);

            double R = qRed(px)   / 255.0 * exposure;
            double G = qGreen(px) / 255.0 * exposure;
            double B = qBlue(px)  / 255.0 * exposure;

            // Rec.2020 luma
            const double L = 0.2627 * R + 0.6780 * G + 0.0593 * B;
            const double scale = 1.0 / (1.0 + L);

            R *= scale;
            G *= scale;
            B *= scale;

            dstLine[x] = qRgba(clamp8(R * 255.0),
                                clamp8(G * 255.0),
                                clamp8(B * 255.0),
                                a);
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// applyToneMapHable
// ---------------------------------------------------------------------------
QImage applyToneMapHable(const QImage& linearRec2020, double exposure)
{
    const QImage src = linearRec2020.convertToFormat(QImage::Format_ARGB32);
    QImage dst(src.size(), QImage::Format_ARGB32);

    // White point normalization factor
    static const double kW         = 11.2;
    static const double kWhiteScale = 1.0 / hableCurve(kW);

    const int w = src.width();
    const int h = src.height();

    for (int y = 0; y < h; ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb*       dstLine = reinterpret_cast<QRgb*>(dst.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int  a  = qAlpha(px);

            double R = qRed(px)   / 255.0 * exposure;
            double G = qGreen(px) / 255.0 * exposure;
            double B = qBlue(px)  / 255.0 * exposure;

            R = hableCurve(R) * kWhiteScale;
            G = hableCurve(G) * kWhiteScale;
            B = hableCurve(B) * kWhiteScale;

            dstLine[x] = qRgba(clamp8(R * 255.0),
                                clamp8(G * 255.0),
                                clamp8(B * 255.0),
                                a);
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// convertColorSpace
// Transfer-function only; color primaries conversion deferred to US-EXT-10.
// ---------------------------------------------------------------------------

// Apply inverse transfer (EOTF) to get linear [0..1]
static double applyEotf(double v, TransferFn fn)
{
    switch (fn) {
    case TransferFn::Linear:
        return v;
    case TransferFn::SDR_Gamma22:
        return std::pow(std::max(v, 0.0), 2.2);
    case TransferFn::PQ_HDR10:
        return static_cast<double>(pqEotf(static_cast<float>(v)));
    case TransferFn::HLG:
        return static_cast<double>(hlgEotf(static_cast<float>(v)));
    }
    return v;
}

// Apply forward transfer (OETF) to get encoded [0..1]
static double applyOetf(double L, TransferFn fn)
{
    switch (fn) {
    case TransferFn::Linear:
        return L;
    case TransferFn::SDR_Gamma22:
        return std::pow(std::max(L, 0.0), 1.0 / 2.2);
    case TransferFn::PQ_HDR10:
        return static_cast<double>(pqOetf(static_cast<float>(L)));
    case TransferFn::HLG:
        return static_cast<double>(hlgOetf(static_cast<float>(L)));
    }
    return L;
}

QImage convertColorSpace(const QImage& src, TransferFn srcFn, TransferFn dstFn)
{
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    QImage out(in.size(), QImage::Format_ARGB32);

    const int w = in.width();
    const int h = in.height();

    for (int y = 0; y < h; ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        QRgb*       dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int  a  = qAlpha(px);

            double R = qRed(px)   / 255.0;
            double G = qGreen(px) / 255.0;
            double B = qBlue(px)  / 255.0;

            // Decode to linear
            R = applyEotf(R, srcFn);
            G = applyEotf(G, srcFn);
            B = applyEotf(B, srcFn);

            // (Color primaries matrix skipped — deferred to US-EXT-10)

            // Encode to destination transfer
            R = applyOetf(R, dstFn);
            G = applyOetf(G, dstFn);
            B = applyOetf(B, dstFn);

            dstLine[x] = qRgba(clamp8(R * 255.0),
                                clamp8(G * 255.0),
                                clamp8(B * 255.0),
                                a);
        }
    }
    return out;
}

} // namespace hdr
