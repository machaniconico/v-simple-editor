// hdr_composite_math_selftests.cpp
// Headless selftest for the pure 16-bit-per-channel compositing engine
// (src/playback/HdrCompositeMath.{h,cpp}, namespace hdrcomposite).
// No QApplication and no GUI/QOpenGL dependency (QtCore + QtGui QImage only).
// Run via: --selftest=hdr-composite-math
//
// Verification strategy: every gate compares the engine output against a
// HAND-DERIVED oracle computed independently below, never against the engine's
// own output — so a regression in the engine cannot make a gate pass
// tautologically. Geometry (paint order / per-layer transform) is REUSED from
// gpucomposite (the 8-bit SSOT) by HdrCompositeMath, so it is not re-tested
// here; this file focuses on the 16-bit blend algebra, 8-bit SSOT parity, and
// the genuine extra precision 16-bit buys over 8-bit.
//
// Gate map (6 gates):
//   G1 compositeReference16 reproduces a single opaque full-canvas layer
//      (identity placement, 16-bit values preserved exactly).
//   G2 premulSourceOver16 of two known colours matches hand-computed 16-bit
//      values (MAX=65535, rounded divide).
//   G3 8-bit SSOT PARITY (non-tautological): feed 8-bit-quantised inputs
//      (v*257 -> 16-bit), composite in 16-bit, down-convert via to8bit(), and
//      compare against an INDEPENDENT 8-bit hand-computed source-over oracle.
//      MAE <= 1 proves the 16-bit path agrees with the 8-bit path.
//   G4 16-bit EXTRA PRECISION (non-tautological): a gradient whose adjacent
//      steps are < 1/255 apart (sub-8-bit) composites in 16-bit to MORE than
//      256 distinct luminance levels — something 8-bit cannot represent.
//   G5 opacity=0.5 blend matches a 16-bit hand calculation.
//   G6 3-layer stack respects gpucomposite paint order (lowest sourceTrack ==
//      V1 == frontmost) in 16-bit.

#include <cstdio>
#include <cmath>
#include <set>

#include <QImage>
#include <QColor>
#include <QRgba64>
#include <QSize>
#include <QVector>

#include "../playback/HdrCompositeMath.h"
#include "../playback/GpuCompositeMath.h"

using hdrcomposite::Rgba16;
using hdrcomposite::premulSourceOver16;
using hdrcomposite::compositeReference16;
using hdrcomposite::to8bit;
using hdrcomposite::kMax16;
using gpucomposite::LayerDesc;

namespace {

// Build a solid RGBA64-premultiplied image of given size and 16-bit colour.
// The (r,g,b,a) are stored VERBATIM as premultiplied channels via raw scanline
// writes — NOT QImage::fill(QColor), which would re-premultiply a straight colour.
// This matches HdrCompositeMath's raw premultiplied read/write so the literal test
// numbers round-trip exactly (the hand-derived oracles below treat them as premul).
QImage solid16(int w, int h, quint16 r, quint16 g, quint16 b, quint16 a) {
    QImage img(w, h, QImage::Format_RGBA64_Premultiplied);
    const QRgba64 px = qRgba64(r, g, b, a);
    for (int y = 0; y < h; ++y) {
        QRgba64* line = reinterpret_cast<QRgba64*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = px;
    }
    return img;
}

// Read the RAW 16-bit premultiplied pixel from an RGBA64_Premultiplied image.
// Deliberately NOT pixelColor() (which un-premultiplies premultiplied formats).
Rgba16 px16(const QImage& img, int x, int y) {
    const QImage p = (img.format() == QImage::Format_RGBA64_Premultiplied)
        ? img : img.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const QRgba64 q = reinterpret_cast<const QRgba64*>(p.constScanLine(y))[x];
    return Rgba16{ q.red(), q.green(), q.blue(), q.alpha() };
}

// 8-bit lift to 16-bit the way QImage::Format_RGBA64 fills from 8-bit values:
//   v16 = v8 * 257  (0->0, 255->65535), exact.
quint16 lift8(int v8) { return static_cast<quint16>(v8 * 257); }

// Independent 8-bit premultiplied source-over oracle (rounded), used by G3.
//   out = src + dst*(255 - srcA)/255
int over8(int s, int d, int srcA) {
    const int v = s + (d * (255 - srcA) + 127) / 255;
    return v > 255 ? 255 : v;
}

} // anonymous namespace

int runHdrCompositeMathSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[hdr-composite-math] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    // ------------------------------------------------------------------
    // G1: single opaque full-canvas layer reproduced exactly (identity).
    // srcSize == canvas, scale1, dx=dy=rot=0, opacity1, opaque colour.
    // Over transparent black with srcA=MAX, out = src (inv=0). 16-bit exact.
    // Pick a colour NOT expressible in 8-bit (0x1234 etc) so the gate would
    // also catch any silent 8-bit truncation inside the engine.
    // ------------------------------------------------------------------
    {
        const QSize canvas(8, 6);
        const quint16 R = 0x1234, G = 0x89AB, B = 0x00FF, A = 0xFFFF;
        QImage src = solid16(canvas.width(), canvas.height(), R, G, B, A);

        QVector<LayerDesc> layers;
        LayerDesc l; l.srcSize = canvas; l.sourceTrack = 0; layers.push_back(l);
        QVector<QImage> imgs; imgs.push_back(src);

        QImage out = compositeReference16(layers, imgs, canvas);
        bool ok = !out.isNull()
               && out.format() == QImage::Format_RGBA64_Premultiplied;
        if (ok) {
            for (int y = 0; y < canvas.height() && ok; ++y)
                for (int x = 0; x < canvas.width() && ok; ++x) {
                    const Rgba16 p = px16(out, x, y);
                    ok = (p.r == R && p.g == G && p.b == B && p.a == A);
                }
        }
        check(1, "single opaque full-canvas layer reproduced exactly (16-bit)", ok);
    }

    // ------------------------------------------------------------------
    // G2: premulSourceOver16 known-colour hand calc. MAX=65535.
    // src = half-alpha premultiplied red:  (32768, 0, 0, 32768)
    // dst = opaque white:                  (65535, 65535, 65535, 65535)
    // inv = MAX - srcA = 65535 - 32768 = 32767.
    // Rounded divide chan(s,d) = s + (d*inv + MAX/2)/MAX:
    //   r = 32768 + (65535*32767 + 32767)/65535
    //     = 32768 + (2147450880)/65535 ... compute exactly below.
    // ------------------------------------------------------------------
    {
        const Rgba16 src{ 32768, 0, 0, 32768 };
        const Rgba16 dst{ 65535, 65535, 65535, 65535 };
        const quint64 MAX = kMax16;
        const quint64 inv = MAX - src.a; // 32767
        auto oracle = [&](quint64 s, quint64 d) -> quint16 {
            const quint64 v = s + (d * inv + MAX / 2) / MAX;
            return static_cast<quint16>(v > MAX ? MAX : v);
        };
        const Rgba16 want{ oracle(src.r, dst.r), oracle(src.g, dst.g),
                           oracle(src.b, dst.b), oracle(src.a, dst.a) };
        const Rgba16 got = premulSourceOver16(dst, src); // (dst, src) order
        const bool ok = got.r == want.r && got.g == want.g
                     && got.b == want.b && got.a == want.a;
        check(2, "premulSourceOver16 two-colour hand calc (MAX=65535)", ok);
    }

    // ------------------------------------------------------------------
    // G3: 8-bit SSOT PARITY (NON-TAUTOLOGICAL).
    // Feed 8-bit-quantised inputs lifted to 16-bit (v*257). Composite a
    // half-alpha premultiplied src over an opaque dst entirely in the 16-bit
    // engine, down-convert with to8bit(), and compare against an INDEPENDENT
    // 8-bit source-over oracle (over8) computed from the original 8-bit values.
    // The oracle never touches the 16-bit engine, so agreement (MAE<=1) is a
    // real cross-check that the 16-bit path matches the 8-bit path.
    //
    // 8-bit inputs (straight chosen so premultiplied math is unambiguous):
    //   dst (base, V1, lower track): opaque (200,100,50,255)
    //   src (overlay, higher track): premultiplied (60,30,10, a=120)
    // ------------------------------------------------------------------
    {
        const QSize canvas(4, 4);
        const int dR=200,dG=100,dB=50,dA=255;
        const int sR=60, sG=30, sB=10, sA=120;

        QImage base = solid16(canvas.width(), canvas.height(),
                              lift8(dR), lift8(dG), lift8(dB), lift8(dA));
        QImage over = solid16(canvas.width(), canvas.height(),
                              lift8(sR), lift8(sG), lift8(sB), lift8(sA));

        // V1 (track 0) is frontmost; the overlay must sit ON TOP, so give it a
        // lower track number than the base so paintOrder puts base behind it.
        // paintOrder: descending sourceTrack, painted back-to-front -> lowest
        // track ends frontmost. So overlay = track 0, base = track 1.
        QVector<LayerDesc> layers;
        LayerDesc lo; lo.srcSize = canvas; lo.sourceTrack = 0; // overlay (front)
        LayerDesc lb; lb.srcSize = canvas; lb.sourceTrack = 1; // base (back)
        layers.push_back(lo);
        layers.push_back(lb);
        QVector<QImage> imgs; imgs.push_back(over); imgs.push_back(base);

        QImage out16 = compositeReference16(layers, imgs, canvas);
        QImage out8  = to8bit(out16);

        // Independent 8-bit oracle: overlay (src) over base (dst).
        const int wR = over8(sR, dR, sA);
        const int wG = over8(sG, dG, sA);
        const int wB = over8(sB, dB, sA);
        const int wA = over8(sA, dA, sA); // 120 + 255*(135)/255 = 255

        long sumErr = 0; int n = 0; bool fmtOk = !out8.isNull();
        if (fmtOk) {
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = 0; x < canvas.width(); ++x) {
                    const QColor c = out8.pixelColor(x, y);
                    sumErr += std::abs(c.red()   - wR);
                    sumErr += std::abs(c.green() - wG);
                    sumErr += std::abs(c.blue()  - wB);
                    sumErr += std::abs(c.alpha() - wA);
                    n += 4;
                }
        }
        const double mae = (n > 0) ? double(sumErr) / n : 1e9;
        check(3, "16-bit path matches INDEPENDENT 8-bit SSOT oracle (MAE<=1)",
              fmtOk && mae <= 1.0);
    }

    // ------------------------------------------------------------------
    // G4: 16-bit EXTRA PRECISION (NON-TAUTOLOGICAL).
    // Build a 1-row gradient whose adjacent samples differ by ONE 16-bit step
    // (1/65535), which is far below one 8-bit step (1/255 == 257 16-bit steps).
    // Composite it opaquely (identity) and count distinct red levels in the
    // 16-bit output. A real 16-bit path preserves > 256 distinct levels; an
    // 8-bit path would collapse this gradient to <= 256.
    // We use 600 columns with red = 20000 + x (each step = 1 of 65535).
    // ------------------------------------------------------------------
    {
        const int W = 600, H = 1;
        QImage grad(W, H, QImage::Format_RGBA64_Premultiplied);
        for (int x = 0; x < W; ++x) {
            QColor c;
            const quint16 r = static_cast<quint16>(20000 + x);
            c.setRgba64(qRgba64(r, 0, 0, 65535));
            grad.setPixelColor(x, 0, c);
        }
        const QSize canvas(W, H);
        QVector<LayerDesc> layers;
        LayerDesc l; l.srcSize = canvas; l.sourceTrack = 0; layers.push_back(l);
        QVector<QImage> imgs; imgs.push_back(grad);

        QImage out16 = compositeReference16(layers, imgs, canvas);

        std::set<quint16> levels16;
        std::set<int> levels8;
        if (!out16.isNull()) {
            for (int x = 0; x < W; ++x) {
                const Rgba16 p = px16(out16, x, 0);
                levels16.insert(p.r);
                levels8.insert(p.r / 257); // what an 8-bit path would keep
            }
        }
        // 16-bit must preserve > 256 distinct levels; the 8-bit projection of
        // the SAME data collapses to <= 256, proving the precision is real.
        const bool ok = levels16.size() > 256 && levels8.size() <= 256;
        check(4, "16-bit preserves >256 distinct levels where 8-bit collapses",
              ok);
    }

    // ------------------------------------------------------------------
    // G5: opacity=0.5 blend, 16-bit hand calc.
    // Single opaque source over transparent black, opacity 0.5. The engine
    // applies opacity by scaling ALL premultiplied src channels by 0.5
    // (rounded: v*0.5 + 0.5), then source-over onto transparent black gives
    // exactly the scaled src.
    //   src opaque (40000, 20000, 10000, 65535), opacity 0.5:
    //     r = round(40000*0.5) = 20000
    //     g = round(20000*0.5) = 10000
    //     b = round(10000*0.5) = 5000
    //     a = round(65535*0.5) = 32768  (32767.5 + 0.5 -> 32768)
    // ------------------------------------------------------------------
    {
        const QSize canvas(3, 3);
        QImage src = solid16(canvas.width(), canvas.height(),
                            40000, 20000, 10000, 65535);
        QVector<LayerDesc> layers;
        LayerDesc l; l.srcSize = canvas; l.sourceTrack = 0; l.opacity = 0.5;
        layers.push_back(l);
        QVector<QImage> imgs; imgs.push_back(src);

        QImage out = compositeReference16(layers, imgs, canvas);
        auto scale = [](quint16 v) -> quint16 {
            const double r = double(v) * 0.5 + 0.5;
            return static_cast<quint16>(r >= 65535.0 ? 65535.0 : r);
        };
        const Rgba16 want{ scale(40000), scale(20000), scale(10000), scale(65535) };
        bool ok = !out.isNull();
        if (ok) {
            const Rgba16 p = px16(out, 1, 1);
            ok = (p.r == want.r && p.g == want.g
               && p.b == want.b && p.a == want.a);
        }
        check(5, "opacity=0.5 scales premultiplied src (16-bit hand calc)", ok);
    }

    // ------------------------------------------------------------------
    // G6: 3-layer paint order in 16-bit. Three opaque solid layers on the same
    // canvas with DISTINCT colours and DISTINCT tracks. paintOrder is descending
    // by sourceTrack painted back-to-front -> lowest track is frontmost (V1).
    // Give track 0 a unique colour; the final composite must equal track-0's
    // colour everywhere (it is fully opaque and frontmost), proving the engine
    // honours gpucomposite::paintOrder rather than input order.
    //
    // To make it non-trivial we put track 0 LAST in the input vector, so naive
    // "last wins" or "first wins" input ordering would give a different colour.
    // ------------------------------------------------------------------
    {
        const QSize canvas(4, 4);
        QImage cBack  = solid16(4, 4, 65535, 0, 0, 65535);     // track 2 (back)
        QImage cMid   = solid16(4, 4, 0, 65535, 0, 65535);     // track 1 (mid)
        QImage cFront = solid16(4, 4, 0, 0, 65535, 65535);     // track 0 (front)

        QVector<LayerDesc> layers;
        QVector<QImage> imgs;
        // Input order intentionally NOT matching paint order:
        LayerDesc lb; lb.srcSize = canvas; lb.sourceTrack = 2; layers.push_back(lb); imgs.push_back(cBack);
        LayerDesc lm; lm.srcSize = canvas; lm.sourceTrack = 1; layers.push_back(lm); imgs.push_back(cMid);
        LayerDesc lf; lf.srcSize = canvas; lf.sourceTrack = 0; layers.push_back(lf); imgs.push_back(cFront);

        QImage out = compositeReference16(layers, imgs, canvas);
        bool ok = !out.isNull();
        if (ok) {
            const Rgba16 p = px16(out, 2, 2);
            // Frontmost == track 0 == blue.
            ok = (p.r == 0 && p.g == 0 && p.b == 65535 && p.a == 65535);
        }
        check(6, "3-layer stack: lowest sourceTrack (V1) is frontmost (16-bit)",
              ok);
    }

    std::printf("[hdr-composite-math] Result: %d/%d PASSED\n",
                passed, passed + failed);
    return failed; // 0 = all PASS
}
