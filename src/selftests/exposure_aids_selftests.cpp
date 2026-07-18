// =============================================================================
//  exposure_aids_selftests.cpp
// -----------------------------------------------------------------------------
//  Headless self-test for the exposureaid:: pure engine (src/ExposureAids.h).
//  QApplication not required: exposureaid::apply / luma709 depend only on
//  QtGui (QImage / QColor / QRgb) and have no global / device state.
//
//  Oracle design (tautology avoidance)
//  -----------------------------------
//   * luma709 is checked against a hand-computed Rec.709 expectation, not via
//     the engine itself.
//   * FalseColor / Zebra / FocusPeaking are validated by their *invariants*
//     (identity passthrough, palette membership, stripe periodicity, edge vs.
//     flat behaviour, dimension preservation, out-of-bounds safety) rather
//     than by re-deriving the result through the function under test.
//
//  Output style mirrors playback_quality_policy_selftests.cpp:
//     "[exposure-aids] Gn (...): PASS/FAIL"  then  "[exposure-aids] Result: N/M PASSED"
// =============================================================================

#include <cstdio>
#include <cmath>
#include <set>

#include <QImage>
#include <QColor>

#include "../ExposureAids.h"

namespace {

using exposureaid::AidMode;
using exposureaid::AidConfig;

// Solid WxH ARGB32 image of one color.
QImage solid(int w, int h, int r, int g, int b, int a = 255)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(qRgba(r, g, b, a));
    return img;
}

bool nearEqual(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

// Count pixels in `out` that differ from the corresponding pixel in `ref`.
int diffCount(const QImage& ref, const QImage& out)
{
    if (ref.size() != out.size()) return -1;
    int n = 0;
    for (int y = 0; y < ref.height(); ++y)
        for (int x = 0; x < ref.width(); ++x)
            if (ref.pixel(x, y) != out.pixel(x, y)) ++n;
    return n;
}

// True if `out` is pixel-identical to `ref` (ignoring format, comparing RGB).
bool sameRgb(const QImage& ref, const QImage& out)
{
    if (ref.size() != out.size()) return false;
    for (int y = 0; y < ref.height(); ++y) {
        for (int x = 0; x < ref.width(); ++x) {
            QRgb a = ref.pixel(x, y), b = out.pixel(x, y);
            if (qRed(a) != qRed(b) || qGreen(a) != qGreen(b) || qBlue(a) != qBlue(b))
                return false;
        }
    }
    return true;
}

} // anonymous namespace

int runExposureAidsSelftest()
{
    int passed = 0;
    int failed = 0;

    const AidConfig kDefault; // zebraThresholdIre=95, zebraStripePx=6, peakingSensitivity=0.20

    // --- Gate 1: luma709 photometric mapping (hand-derived oracle) -----------
    {
        double white = exposureaid::luma709(255, 255, 255);
        double black = exposureaid::luma709(0, 0, 0);
        // R-only: 0.2126 * (255/255) = 0.2126
        double rOnly = exposureaid::luma709(255, 0, 0);
        double gOnly = exposureaid::luma709(0, 255, 0); // 0.7152
        double bOnly = exposureaid::luma709(0, 0, 255); // 0.0722
        bool ok = nearEqual(white, 1.0) && nearEqual(black, 0.0)
               && nearEqual(rOnly, 0.2126) && nearEqual(gOnly, 0.7152)
               && nearEqual(bOnly, 0.0722);
        std::printf("[exposure-aids] Gate 1 (luma709 Rec.709 mapping): %s "
                    "(white=%.4f black=%.4f R=%.4f G=%.4f B=%.4f)\n",
                    ok ? "PASS" : "FAIL", white, black, rOnly, gOnly, bOnly);
        ok ? ++passed : ++failed;
    }

    // --- Gate 2: None mode is identity --------------------------------------
    {
        QImage in = solid(8, 8, 30, 120, 200);
        QImage out = exposureaid::apply(in, AidMode::None, kDefault);
        bool ok = sameRgb(in, out);
        std::printf("[exposure-aids] Gate 2 (None mode is identity): %s (diff=%d)\n",
                    ok ? "PASS" : "FAIL", diffCount(in, out));
        ok ? ++passed : ++failed;
    }

    // --- Gate 3: FalseColor extremes -> distinct lowest/highest zone colors --
    // Oracle: black (luma 0) must land in the darkest zone, white (luma 1,
    // i.e. clip) in the brightest. We do not hardcode the exact triples; we
    // assert (a) black != white output, (b) black output != black input,
    // (c) white output != white input (palette recolor happened), and
    // (d) both are saturated palette entries, not the source pixels.
    {
        QImage blackOut = exposureaid::apply(solid(4, 4, 0, 0, 0),       AidMode::FalseColor, kDefault);
        QImage whiteOut = exposureaid::apply(solid(4, 4, 255, 255, 255), AidMode::FalseColor, kDefault);
        QRgb lo = blackOut.pixel(0, 0);
        QRgb hi = whiteOut.pixel(0, 0);
        bool loRecolored = !(qRed(lo) == 0 && qGreen(lo) == 0 && qBlue(lo) == 0);
        bool hiRecolored = !(qRed(hi) == 255 && qGreen(hi) == 255 && qBlue(hi) == 255);
        bool distinct = (lo != hi);
        // Highest zone should be the warmest (red-dominant clip warning).
        bool hiWarm = qRed(hi) >= qGreen(hi) && qRed(hi) >= qBlue(hi);
        bool ok = loRecolored && hiRecolored && distinct && hiWarm;
        std::printf("[exposure-aids] Gate 3 (FalseColor low/high zones): %s "
                    "(lo=%d,%d,%d hi=%d,%d,%d)\n", ok ? "PASS" : "FAIL",
                    qRed(lo), qGreen(lo), qBlue(lo), qRed(hi), qGreen(hi), qBlue(hi));
        ok ? ++passed : ++failed;
    }

    // --- Gate 4: FalseColor correct-exposure band -> green-dominant ----------
    // A mid grey with luma ~0.45 (IRE ~45) sits in the "correct exposure" band,
    // which by NLE convention is green. Pick a grey whose luma709 ~ 0.45.
    {
        // grey value v: luma = v/255. For IRE ~45 -> v ~ 115.
        int v = 115;
        double l = exposureaid::luma709(v, v, v); // ~0.451
        QImage out = exposureaid::apply(solid(4, 4, v, v, v), AidMode::FalseColor, kDefault);
        QRgb p = out.pixel(0, 0);
        bool greenDominant = qGreen(p) > qRed(p) && qGreen(p) > qBlue(p);
        bool ok = greenDominant;
        std::printf("[exposure-aids] Gate 4 (FalseColor correct-exposure=green): %s "
                    "(luma=%.3f -> %d,%d,%d)\n", ok ? "PASS" : "FAIL",
                    l, qRed(p), qGreen(p), qBlue(p));
        ok ? ++passed : ++failed;
    }

    // --- Gate 5: FalseColor output uses only a small fixed palette ----------
    // A smooth ramp must collapse to a handful of distinct zone colors, and
    // none of those colors may equal the original (the source is fully recolored
    // for at least the extremes). We assert the distinct-color count is small
    // (<= 16 zones) and far below the input's distinct-color count.
    {
        QImage ramp(64, 1, QImage::Format_ARGB32);
        std::set<QRgb> inColors;
        for (int x = 0; x < 64; ++x) {
            int v = x * 4; // 0..252
            ramp.setPixel(x, 0, qRgb(v, v, v));
            inColors.insert(qRgb(v, v, v));
        }
        QImage out = exposureaid::apply(ramp, AidMode::FalseColor, kDefault);
        std::set<QRgb> outColors;
        for (int x = 0; x < 64; ++x)
            outColors.insert(out.pixel(x, 0) | 0xFF000000u); // ignore alpha
        bool palettized = outColors.size() <= 16 && outColors.size() < inColors.size();
        bool ok = palettized;
        std::printf("[exposure-aids] Gate 5 (FalseColor palettizes ramp): %s "
                    "(in=%zu distinct, out=%zu distinct)\n", ok ? "PASS" : "FAIL",
                    inColors.size(), outColors.size());
        ok ? ++passed : ++failed;
    }

    // --- Gate 6: Zebra stripes bright, leaves dark untouched -----------------
    // Top half = bright (>= threshold), bottom half = dark (< threshold).
    {
        QImage img(16, 16, QImage::Format_ARGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                img.setPixel(x, y, (y < 8) ? qRgb(255, 255, 255) : qRgb(20, 20, 20));
        QImage out = exposureaid::apply(img, AidMode::Zebra, kDefault);

        // Dark region: completely unchanged.
        bool darkUntouched = true;
        for (int y = 8; y < 16 && darkUntouched; ++y)
            for (int x = 0; x < 16; ++x)
                if (out.pixel(x, y) != img.pixel(x, y)) { darkUntouched = false; break; }

        // Bright region: some pixels changed (striped) AND some unchanged
        // (passthrough band) => actual stripes, not a full fill.
        int changed = 0, kept = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                (out.pixel(x, y) != img.pixel(x, y)) ? ++changed : ++kept;
        bool striped = (changed > 0 && kept > 0);

        bool ok = darkUntouched && striped;
        std::printf("[exposure-aids] Gate 6 (Zebra: bright striped, dark untouched): %s "
                    "(brightChanged=%d brightKept=%d darkUntouched=%d)\n",
                    ok ? "PASS" : "FAIL", changed, kept, (int)darkUntouched);
        ok ? ++passed : ++failed;
    }

    // --- Gate 7: zebraStripePx changes the stripe period ---------------------
    // A larger stripe period must produce a different (coarser) pattern.
    {
        QImage bright = solid(32, 4, 255, 255, 255);
        AidConfig narrow = kDefault; narrow.zebraStripePx = 2;
        AidConfig wide   = kDefault; wide.zebraStripePx   = 10;
        QImage outN = exposureaid::apply(bright, AidMode::Zebra, narrow);
        QImage outW = exposureaid::apply(bright, AidMode::Zebra, wide);
        // Count transitions along a scanline (proxy for stripe frequency).
        auto transitions = [](const QImage& im, int y) {
            int t = 0;
            for (int x = 1; x < im.width(); ++x)
                if (im.pixel(x, y) != im.pixel(x - 1, y)) ++t;
            return t;
        };
        int tN = transitions(outN, 0);
        int tW = transitions(outW, 0);
        bool ok = (tN != tW) && (tN > tW); // narrower period => more transitions
        std::printf("[exposure-aids] Gate 7 (zebraStripePx alters period): %s "
                    "(transitions narrow=%d wide=%d)\n", ok ? "PASS" : "FAIL", tN, tW);
        ok ? ++passed : ++failed;
    }

    // --- Gate 8: FocusPeaking tints edges, not flat regions ------------------
    // Vertical edge: left half black, right half white.
    {
        const int W = 20, H = 8;
        QImage img(W, H, QImage::Format_ARGB32);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                img.setPixel(x, y, (x < W / 2) ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
        QImage out = exposureaid::apply(img, AidMode::FocusPeaking, kDefault);

        QColor pk = kDefault.peakingColor;
        auto isPeak = [&](QRgb p) {
            return qRed(p) == pk.red() && qGreen(p) == pk.green() && qBlue(p) == pk.blue();
        };

        // Edge column band (around x == W/2 - 1) should contain peaking tint.
        int edgeTinted = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = W / 2 - 2; x <= W / 2; ++x) {
                if (x >= 0 && x < W && isPeak(out.pixel(x, y))) { ++edgeTinted; break; }
            }
        }
        // Deep-flat columns (far from edge) must NOT be peaking-tinted.
        int flatTinted = 0;
        for (int y = 0; y < H; ++y) {
            if (isPeak(out.pixel(2, y)))       ++flatTinted; // deep left
            if (isPeak(out.pixel(W - 2, y)))   ++flatTinted; // deep right
        }
        bool ok = (edgeTinted > 0) && (flatTinted == 0);
        std::printf("[exposure-aids] Gate 8 (FocusPeaking tints edge, not flat): %s "
                    "(edgeTinted=%d flatTinted=%d)\n", ok ? "PASS" : "FAIL",
                    edgeTinted, flatTinted);
        ok ? ++passed : ++failed;
    }

    // --- Gate 9: higher peakingSensitivity => fewer tinted pixels ------------
    // Build a soft gradient so a range of edge strengths exist; raising the
    // threshold must monotonically reduce the number of tinted pixels.
    {
        const int W = 64, H = 8;
        QImage grad(W, H, QImage::Format_ARGB32);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int v = (x * 255) / (W - 1);
                grad.setPixel(x, y, qRgb(v, v, v));
            }
        AidConfig lowSens  = kDefault; lowSens.peakingSensitivity  = 0.02;
        AidConfig highSens = kDefault; highSens.peakingSensitivity = 0.60;
        QImage outLow  = exposureaid::apply(grad, AidMode::FocusPeaking, lowSens);
        QImage outHigh = exposureaid::apply(grad, AidMode::FocusPeaking, highSens);
        QColor pk = kDefault.peakingColor;
        auto countPeak = [&](const QImage& im) {
            int n = 0;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x) {
                    QRgb p = im.pixel(x, y);
                    if (qRed(p) == pk.red() && qGreen(p) == pk.green() && qBlue(p) == pk.blue())
                        ++n;
                }
            return n;
        };
        int nLow  = countPeak(outLow);
        int nHigh = countPeak(outHigh);
        bool ok = nHigh <= nLow; // monotonic: higher threshold never tints more
        std::printf("[exposure-aids] Gate 9 (higher sensitivity -> fewer tints): %s "
                    "(tinted low=%d high=%d)\n", ok ? "PASS" : "FAIL", nLow, nHigh);
        ok ? ++passed : ++failed;
    }

    // --- Gate 10: tiny / degenerate images do not crash (edge sampling) ------
    {
        bool ok = true;
        const AidMode modes[] = { AidMode::None, AidMode::FalseColor,
                                  AidMode::Zebra, AidMode::FocusPeaking };
        const int sizes[][2] = { {1, 1}, {2, 2}, {1, 5}, {5, 1} };
        for (auto& s : sizes) {
            QImage in = solid(s[0], s[1], 200, 50, 50);
            for (auto m : modes) {
                QImage out = exposureaid::apply(in, m, kDefault);
                if (out.width() != s[0] || out.height() != s[1] || out.isNull()) {
                    ok = false;
                }
            }
        }
        // Also: a null image must round-trip without crashing.
        QImage nullIn;
        QImage nullOut = exposureaid::apply(nullIn, AidMode::FocusPeaking, kDefault);
        (void)nullOut;
        std::printf("[exposure-aids] Gate 10 (1x1/2x2/strip + null images safe): %s\n",
                    ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // --- Gate 11: output dimensions == input dimensions (all modes) ---------
    {
        QImage in = solid(13, 7, 90, 160, 40);
        bool ok = true;
        const AidMode modes[] = { AidMode::None, AidMode::FalseColor,
                                  AidMode::Zebra, AidMode::FocusPeaking };
        for (auto m : modes) {
            QImage out = exposureaid::apply(in, m, kDefault);
            if (out.width() != in.width() || out.height() != in.height()) {
                ok = false;
                std::printf("[exposure-aids]   mode %d -> %dx%d (expected %dx%d)\n",
                            (int)m, out.width(), out.height(), in.width(), in.height());
            }
        }
        std::printf("[exposure-aids] Gate 11 (output size == input size, all modes): %s\n",
                    ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    std::printf("[exposure-aids] Result: %d/%d PASSED\n", passed, passed + failed);
    return failed; // 0 = all PASS
}
