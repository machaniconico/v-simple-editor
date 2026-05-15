#include "HdrGrading.h"

#include <QImage>
#include <cmath>
#include <algorithm>

namespace hdr {

namespace {

inline double clamp01(double x)
{
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

// PQ constants (SMPTE ST 2084).
constexpr double kPqM1 = 0.1593017578125;
constexpr double kPqM2 = 78.84375;
constexpr double kPqC1 = 0.8359375;
constexpr double kPqC2 = 18.8515625;
constexpr double kPqC3 = 18.6875;

// HLG constants (ARIB STD-B67 / Rec.2100).
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;

// Reinhard tone-map operator.
double reinhard(double x)
{
    if (x < 0.0) x = 0.0;
    return x / (1.0 + x);
}

// ACES filmic approximation (Narkowicz fit).
double acesFilmic(double x)
{
    if (x < 0.0) x = 0.0;
    const double num = x * (2.51 * x + 0.03);
    const double den = x * (2.43 * x + 0.59) + 0.14;
    if (den <= 0.0)
        return 0.0;
    return clamp01(num / den);
}

double applyOperator(ToneMapOperator op, double x)
{
    switch (op) {
    case ToneMapOperator::Reinhard:
        return reinhard(x);
    case ToneMapOperator::AcesFilmic:
    default:
        return acesFilmic(x);
    }
}

// Encode a linear [0,1] value to 8-bit using a 1/2.2 gamma curve.
int encodeGamma8(double linear)
{
    linear = clamp01(linear);
    const double enc = std::pow(linear, 1.0 / 2.2);
    int v = static_cast<int>(std::lround(enc * 255.0));
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return v;
}

} // namespace

double applyPqEotf(double codeValue)
{
    const double V = clamp01(codeValue);

    // num = max(V^(1/m2) - c1, 0)
    // den = c2 - c3 * V^(1/m2)
    // L   = (num / den)^(1/m1)
    const double vp = std::pow(V, 1.0 / kPqM2);

    double num = vp - kPqC1;
    if (num < 0.0)
        num = 0.0;

    double den = kPqC2 - kPqC3 * vp;
    if (den <= 0.0)
        return 0.0; // guard; only reachable for V outside [0,1]

    const double L = std::pow(num / den, 1.0 / kPqM1);
    return clamp01(L);
}

double applyHlgOetf(double linear)
{
    const double x = clamp01(linear);

    // x <= 1/12 : sqrt(3x)
    // x  > 1/12 : a*ln(12x - b) + c
    if (x <= 1.0 / 12.0)
        return clamp01(std::sqrt(3.0 * x));

    return clamp01(kHlgA * std::log(12.0 * x - kHlgB) + kHlgC);
}

QImage toneMapHdrToSdr(const QImage &hdrLinear,
                       TransferFunction src,
                       ToneMapOperator op,
                       double maxNits)
{
    if (hdrLinear.isNull())
        return QImage();

    const QImage in = hdrLinear.convertToFormat(QImage::Format_ARGB32);
    const int w = in.width();
    const int h = in.height();

    QImage out(w, h, QImage::Format_RGB32);

    if (maxNits <= 0.0)
        maxNits = 1000.0;

    // PQ EOTF returns a fraction of 10000 nits. We rescale so that the
    // chosen display/master peak (maxNits) maps to ~1.0 before tone mapping.
    const double pqScale = 10000.0 / maxNits;

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(in.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(out.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];

            double rr = qRed(px)   / 255.0;
            double gg = qGreen(px) / 255.0;
            double bb = qBlue(px)  / 255.0;

            switch (src) {
            case TransferFunction::PQ_Rec2100:
                // Decode PQ to a 10000-nit fraction, then rescale so that
                // `maxNits` is the new 1.0 reference.
                rr = applyPqEotf(rr) * pqScale;
                gg = applyPqEotf(gg) * pqScale;
                bb = applyPqEotf(bb) * pqScale;
                break;

            case TransferFunction::HLG_Rec2100:
                // The stored value is treated as the HLG signal. Inverting
                // the OETF exactly requires solving a transcendental for the
                // upper segment, so we keep it simple and use the square of
                // the signal as a cheap, monotonic display-linear proxy,
                // then scale to the chosen peak (in 10000-nit fractions).
                rr = (rr * rr) * (maxNits / 10000.0) * pqScale;
                gg = (gg * gg) * (maxNits / 10000.0) * pqScale;
                bb = (bb * bb) * (maxNits / 10000.0) * pqScale;
                break;

            case TransferFunction::SDR_Rec709:
            default:
                // Already (roughly) linear; pass through unchanged.
                break;
            }

            const double tr = applyOperator(op, rr);
            const double tg = applyOperator(op, gg);
            const double tb = applyOperator(op, bb);

            dstLine[x] = qRgb(encodeGamma8(tr),
                              encodeGamma8(tg),
                              encodeGamma8(tb));
        }
    }

    return out;
}

} // namespace hdr
