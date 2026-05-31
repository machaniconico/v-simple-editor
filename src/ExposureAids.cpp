// =============================================================================
//  ExposureAids.cpp
// -----------------------------------------------------------------------------
//  Implementation of the preview-only exposure / focus monitoring aids.
//  See ExposureAids.h for the full design contract (preview-only, pure,
//  deterministic, side-effect free).
// =============================================================================

#include "ExposureAids.h"

#include <algorithm>
#include <cmath>

namespace exposureaid {

// ---------------------------------------------------------------------------
//  Rec.709 luma
// ---------------------------------------------------------------------------
double luma709(int r, int g, int b)
{
    // Clamp defensively so out-of-range callers cannot produce >1 / <0 luma.
    const double rn = std::clamp(r, 0, 255) / 255.0;
    const double gn = std::clamp(g, 0, 255) / 255.0;
    const double bn = std::clamp(b, 0, 255) / 255.0;
    return 0.2126 * rn + 0.7152 * gn + 0.0722 * bn;
}

namespace {

// ---------------------------------------------------------------------------
//  False Color palette
// ---------------------------------------------------------------------------
//  Each entry maps an inclusive lower luma bound (in 0..100 "IRE" units) to a
//  fixed display color. A pixel is colored by the LAST zone whose `loIre` is
//  <= its luma*100. Zones are listed in ascending order. The original pixel
//  color is discarded (matching how NLE false-color works).
//
//  Zone scheme (approx. Resolve / ARRI style):
//    0.0 - 2.5%   crushed blacks        -> purple
//    2.5 - 10%    deep shadow           -> blue
//    10  - 40%    low mids              -> teal/cyan
//    40  - 50%    proper mid-gray       -> green   (correct exposure band)
//    50  - 70%    upper mids            -> gray
//    70  - 90%    skin/highlights       -> pink
//    90  - 95%    bright highlights     -> yellow
//    95  - 100%   near clipping         -> orange
//    100% (exact) clipped white         -> red
struct FalseColorZone {
    double loIre;   // inclusive lower bound, 0..100
    QRgb   color;
};

constexpr FalseColorZone kFalseColorZones[] = {
    {  0.0, qRgb(128,   0, 200) }, // purple  - crushed blacks
    {  2.5, qRgb(  0,   0, 255) }, // blue    - deep shadow
    { 10.0, qRgb(  0, 180, 200) }, // teal    - low mids
    { 40.0, qRgb(  0, 200,   0) }, // green   - proper mid exposure
    { 50.0, qRgb(160, 160, 160) }, // gray    - upper mids
    { 70.0, qRgb(255, 120, 200) }, // pink    - skin / highlights
    { 90.0, qRgb(255, 255,   0) }, // yellow  - bright highlights
    { 95.0, qRgb(255, 150,   0) }, // orange  - near clipping
};
// Exact-clip (luma*100 >= 100) is special-cased to red below.
constexpr QRgb kClipColor = qRgb(255, 0, 0);

QRgb falseColorFor(double lumaIre)
{
    if (lumaIre >= 100.0)
        return kClipColor; // clipped white -> red

    QRgb chosen = kFalseColorZones[0].color;
    for (const auto& z : kFalseColorZones) {
        if (lumaIre >= z.loIre)
            chosen = z.color;
        else
            break; // zones are ascending; no later zone can match
    }
    return chosen;
}

// Normalize any input format to ARGB32 so pixel access is uniform.
QImage toArgb32(const QImage& src)
{
    if (src.format() == QImage::Format_ARGB32)
        return src.copy(); // detach: never alias caller's buffer
    return src.convertToFormat(QImage::Format_ARGB32);
}

} // namespace

// ---------------------------------------------------------------------------
//  apply
// ---------------------------------------------------------------------------
QImage apply(const QImage& src, AidMode mode, const AidConfig& cfg)
{
    // Null / empty image: nothing to do, hand back as-is.
    if (src.isNull())
        return src;

    // Identity: return an unchanged copy of the source (original format kept).
    if (mode == AidMode::None)
        return src.copy();

    const QImage in = toArgb32(src);
    const int w = in.width();
    const int h = in.height();

    QImage out(w, h, QImage::Format_ARGB32);

    switch (mode) {
    // -------------------------------------------------------------------
    //  False Color: every pixel replaced by its IRE-zone color (alpha kept).
    // -------------------------------------------------------------------
    case AidMode::FalseColor: {
        for (int y = 0; y < h; ++y) {
            const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
            QRgb* dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = srcLine[x];
                const double lumaIre = luma709(qRed(p), qGreen(p), qBlue(p)) * 100.0;
                const QRgb c = falseColorFor(lumaIre);
                dstLine[x] = qRgba(qRed(c), qGreen(c), qBlue(c), qAlpha(p));
            }
        }
        break;
    }

    // -------------------------------------------------------------------
    //  Zebra: diagonal stripes over pixels at/above the IRE threshold.
    //  Phase is fixed (depends only on x,y) so the result is deterministic.
    //  Below threshold -> passthrough. On the stripe band -> black.
    // -------------------------------------------------------------------
    case AidMode::Zebra: {
        // AidConfig::zebraStripePx is the full diagonal cycle (stripe +
        // passthrough), not the width of each half-band.
        const int period = std::max(2, cfg.zebraStripePx);
        const int stripeWidth = std::max(1, period / 2);
        const double thr = cfg.zebraThresholdIre;
        // ゼブラの縞は「白飛び (near-white) 領域」に重ねるため、白い縞では
        // 見えない。NLE 定石どおり暗い斜線 (黒) を使い、明部上で明瞭に出す。
        const QRgb stripeColor = qRgb(0, 0, 0);
        for (int y = 0; y < h; ++y) {
            const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
            QRgb* dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = srcLine[x];
                const double lumaIre = luma709(qRed(p), qGreen(p), qBlue(p)) * 100.0;
                if (lumaIre >= thr) {
                    const int phase = (x + y) % period;
                    if (phase < stripeWidth)
                        dstLine[x] = qRgba(qRed(stripeColor), qGreen(stripeColor),
                                           qBlue(stripeColor), qAlpha(p));
                    else
                        dstLine[x] = p;
                } else {
                    dstLine[x] = p; // unaffected
                }
            }
        }
        break;
    }

    // -------------------------------------------------------------------
    //  Focus Peaking: tint pixels whose local luma gradient exceeds the
    //  sensitivity. Gradient is a cheap Roberts/Sobel-style estimate using
    //  the right and bottom neighbours (one extra pixel each). Edges are
    //  handled safely by reusing the current pixel where a neighbour is OOB,
    //  yielding a zero contribution there. Non-edge pixels keep the source
    //  color but slightly desaturated so the peaking tint stands out.
    // -------------------------------------------------------------------
    case AidMode::FocusPeaking: {
        const double sens = std::clamp(cfg.peakingSensitivity, 0.0, 1.0);
        const int pr = cfg.peakingColor.red();
        const int pg = cfg.peakingColor.green();
        const int pb = cfg.peakingColor.blue();

        for (int y = 0; y < h; ++y) {
            const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
            const QRgb* nextLine =
                (y + 1 < h) ? reinterpret_cast<const QRgb*>(in.constScanLine(y + 1))
                            : srcLine; // bottom edge -> reuse self (zero dY)
            QRgb* dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = srcLine[x];
                const double lc = luma709(qRed(p), qGreen(p), qBlue(p));

                // Right neighbour (right edge -> reuse self, zero dX).
                const QRgb pr_px = (x + 1 < w) ? srcLine[x + 1] : p;
                const QRgb pd_px = nextLine[x];
                const double lr = luma709(qRed(pr_px), qGreen(pr_px), qBlue(pr_px));
                const double ld = luma709(qRed(pd_px), qGreen(pd_px), qBlue(pd_px));

                const double dx = lr - lc;
                const double dy = ld - lc;
                // Magnitude already in 0..1-ish range (luma diffs); clamp.
                const double grad = std::min(1.0, std::sqrt(dx * dx + dy * dy));

                if (grad > sens) {
                    dstLine[x] = qRgba(pr, pg, pb, qAlpha(p)); // tint edge
                } else {
                    // Desaturate the source so tinted edges pop visually.
                    const int gray = static_cast<int>(std::lround(lc * 255.0));
                    const int r = (qRed(p) + gray) / 2;
                    const int g = (qGreen(p) + gray) / 2;
                    const int b = (qBlue(p) + gray) / 2;
                    dstLine[x] = qRgba(r, g, b, qAlpha(p));
                }
            }
        }
        break;
    }

    case AidMode::None:
    default:
        // None handled above; default is a safety identity copy.
        return src.copy();
    }

    return out;
}

} // namespace exposureaid
