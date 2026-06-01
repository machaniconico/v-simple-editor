// src/selftests/hdr_composite_parity_selftests.cpp
// GPU RGBA16 (16-bit) vs CPU 16-bit oracle measured-parity selftest — Stage4C HDR-2.
//
// HDR-1 (hdr-composite-math) proved the PURE 16-bit compositing math headless
// (HdrCompositeMath, namespace hdrcomposite). HDR-2 (this test) drives the REAL
// GPU RGBA16 pipeline — GpuLayerCompositor::composite16(): render N layers into
// an RGBA16 offscreen FBO with premultiplied source-over, read back 16-bit per
// channel via glReadPixels(GL_RGBA, GL_UNSIGNED_SHORT) — against the project's
// pure CPU 16-bit oracle hdrcomposite::compositeReference16().
//
// This is a SECOND, independent GPU path. It does NOT touch the 8-bit
// composite() (gpu-composite-parity gates that). composite16() returns a
// QImage::Format_RGBA64_Premultiplied, or a NULL QImage when GL or the RGBA16
// FBO is unavailable (caller SKIPs).
//
// Matte support: OUT OF SCOPE here — MATTE-FREE only, exactly mirroring
// HdrCompositeMath::compositeReference16. matteType is ignored on BOTH sides; no
// matte-source exclusion; every isLayerComposited layer is painted plainly in
// gpucomposite::paintOrder. (Matte parity is a later HDR phase.)
//
// Inputs are mapped between the two APIs as follows: for each gate we build a
// QVector<GpuLayerInput> for the GPU and an INDEX-ALIGNED
// QVector<gpucomposite::LayerDesc> + QVector<QImage> for the CPU oracle, from the
// SAME source images. Both APIs paint in gpucomposite::paintOrder (DESCENDING
// sourceTrack -> lowest sourceTrack frontmost == V1-wins), so equal inputs must
// produce equal output within tight 16-bit tolerances.
//
// Graceful skip: if no usable GL context exists (headless box / WSL without GL)
// GpuLayerCompositor::isAvailable() returns false; and if the RGBA16 FBO cannot
// be created composite16() returns a null image. In either case we print an
// explicit SKIP line and return 0 — never a silent pass, never a FAIL on missing
// GL, and never a hang.

#include "../playback/GpuLayerCompositor.h"
#include "../playback/GpuCompositeMath.h"
#include "../playback/HdrCompositeMath.h"

#include <QApplication>
#include <QGuiApplication>
#include <QImage>
#include <QColor>
#include <QRgba64>
#include <QSize>
#include <QVector>
#include <QString>

#include <cstdio>
#include <cmath>
#include <set>

namespace {

using gpucomposite::LayerDesc;

// ---------------------------------------------------------------------------
// Source-image builders. All 16-bit (Format_RGBA64) so we exercise the genuine
// RGBA16 upload path. 8-bit gradients are lifted to 16-bit via v*257 (0->0,
// 255->65535, exact), so the CPU oracle and GPU upload see identical 16-bit
// values.
// ---------------------------------------------------------------------------

quint16 lift8(int v8) { return static_cast<quint16>(v8 * 257); }

// A solid RGBA64 image (straight alpha). 8-bit components lifted to 16-bit.
QImage solid16(QSize sz, int r, int g, int b, int a)
{
    QImage img(sz, QImage::Format_RGBA64);
    QColor c;
    c.setRgba64(qRgba64(lift8(r), lift8(g), lift8(b), lift8(a)));
    img.fill(c);
    return img;
}

QImage transparentCanvas16(QSize sz)
{
    QImage img(sz, QImage::Format_RGBA64_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// A deterministic 16-bit gradient so sampling/placement differences show up (a
// flat fill would hide many transform bugs). 8-bit gradient lifted to 16-bit.
QImage gradient16(QSize sz, int rBase, int gBase, int bBase)
{
    QImage img(sz, QImage::Format_RGBA64);
    for (int y = 0; y < sz.height(); ++y) {
        for (int x = 0; x < sz.width(); ++x) {
            const int r = (rBase + x * 3) & 0xFF;
            const int g = (gBase + y * 3) & 0xFF;
            const int b = (bBase + (x + y) * 2) & 0xFF;
            QColor c;
            c.setRgba64(qRgba64(lift8(r), lift8(g), lift8(b), lift8(255)));
            img.setPixelColor(x, y, c);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// 16-bit comparison metrics. rgb16Mae / a16Mae are mean absolute error over the
// full 0..65535 range, read via QColor::rgba64(). ssim is computed on an 8-bit
// projection (convertToFormat(Format_ARGB32)) reusing the gpu-composite-parity
// global luma SSIM formula.
// ---------------------------------------------------------------------------
struct Metrics16 {
    bool   valid = false;
    double ssim = 0.0;       // global luma SSIM on 8-bit projection, -1..1
    double rgb16Mae = 1e18;  // mean abs error over RGB, 0..65535
    double a16Mae = 1e18;    // mean abs error over alpha, 0..65535
};

Metrics16 compare16(const QImage& aIn, const QImage& bIn)
{
    Metrics16 out;
    if (aIn.isNull() || bIn.isNull())
        return out;
    if (aIn.size() != bIn.size())
        return out;

    const int w = aIn.width();
    const int h = aIn.height();
    const double n = double(w) * double(h);
    if (n <= 0.0)
        return out;

    // --- 16-bit MAE: read RAW premultiplied RGBA64 directly so sub-8-bit error is
    // visible and there is NO un-premultiply alpha amplification. (pixelColor() would
    // un-premultiply, dividing by small alpha and exaggerating error on faint pixels.)
    const QImage a16 = aIn.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    const QImage b16 = bIn.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    double rgbSum = 0.0;
    double aSum = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgba64* ra = reinterpret_cast<const QRgba64*>(a16.constScanLine(y));
        const QRgba64* rb = reinterpret_cast<const QRgba64*>(b16.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgba64 pa = ra[x];
            const QRgba64 pb = rb[x];
            rgbSum += std::abs(int(pa.red())   - int(pb.red()));
            rgbSum += std::abs(int(pa.green()) - int(pb.green()));
            rgbSum += std::abs(int(pa.blue())  - int(pb.blue()));
            aSum   += std::abs(int(pa.alpha()) - int(pb.alpha()));
        }
    }
    out.rgb16Mae = rgbSum / (n * 3.0);
    out.a16Mae   = aSum / n;

    // --- SSIM: 8-bit luma projection, same formula as gpu-composite-parity. --
    const QImage a = aIn.convertToFormat(QImage::Format_ARGB32);
    const QImage b = bIn.convertToFormat(QImage::Format_ARGB32);
    double meanA = 0.0, meanB = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pb = rb[x];
            meanA += 0.299 * qRed(pa) + 0.587 * qGreen(pa) + 0.114 * qBlue(pa);
            meanB += 0.299 * qRed(pb) + 0.587 * qGreen(pb) + 0.114 * qBlue(pb);
        }
    }
    meanA /= n;
    meanB /= n;
    double varA = 0.0, varB = 0.0, cov = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pb = rb[x];
            const double la = 0.299 * qRed(pa) + 0.587 * qGreen(pa) + 0.114 * qBlue(pa);
            const double lb = 0.299 * qRed(pb) + 0.587 * qGreen(pb) + 0.114 * qBlue(pb);
            const double da = la - meanA;
            const double db = lb - meanB;
            varA += da * da;
            varB += db * db;
            cov  += da * db;
        }
    }
    varA /= n;
    varB /= n;
    cov  /= n;
    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    out.ssim = ((2.0 * meanA * meanB + C1) * (2.0 * cov + C2)) /
               ((meanA * meanA + meanB * meanB + C1) * (varA + varB + C2));
    out.valid = true;
    return out;
}

// Build a GpuLayerInput (matte-FREE) from a 16-bit source image.
GpuLayerInput makeLayer(const QImage& img, int sourceTrack, double opacity,
                        double scale, double dx, double dy, double rotDeg)
{
    GpuLayerInput in;
    in.image = img;
    in.desc.sourceTrack = sourceTrack;
    in.desc.srcSize = img.size();
    in.desc.opacity = opacity;
    in.desc.videoScale = scale;
    in.desc.videoDx = dx;
    in.desc.videoDy = dy;
    in.desc.rotation2DDegrees = rotDeg;
    in.desc.visible = true;
    in.desc.matteSourceIndex = -1;             // matte-FREE
    in.desc.matteType = gpucomposite::MatteType::None;
    return in;
}

// The matching CPU oracle LayerDesc for a GpuLayerInput (index-aligned).
LayerDesc descFor(const GpuLayerInput& in) { return in.desc; }

} // anonymous namespace

int runHdrCompositeParitySelftest()
{
    // Needs a Q(Gui)Application for the GL context. The registry dispatches this
    // entry AFTER QApplication is constructed (needsQApplication=true), so an
    // instance should already exist; we never construct one here.
    if (!qApp) {
        std::printf("[hdr-composite-parity] SKIP: no QApplication instance (GL unavailable)\n");
        return 0;
    }

    GpuLayerCompositor gpu;
    if (!gpu.isAvailable()) {
        std::printf("[hdr-composite-parity] SKIP: no usable GL context (headless/WSL)\n");
        return 0;
    }

    // Probe: a trivial single-layer 16-bit composite. If composite16 returns a
    // null / size-mismatched image, the RGBA16 FBO is unavailable on this GL —
    // SKIP rather than FAIL. Never hang, never silent-pass.
    const QSize probeCanvas(64, 48);
    {
        QVector<GpuLayerInput> probe;
        probe.push_back(makeLayer(solid16(probeCanvas, 120, 130, 140, 255),
                                  /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        const QImage probeImg = gpu.composite16(probe, probeCanvas);
        if (probeImg.isNull() || probeImg.size() != probeCanvas) {
            std::printf("[hdr-composite-parity] SKIP: RGBA16 FBO unavailable\n");
            return 0;
        }
    }

    int passed = 0;
    int total = 0;

    // gate(): run one parity comparison. Builds the CPU oracle from the SAME
    // GpuLayerInput vector (index-aligned descs + images), runs the GPU 16-bit
    // path, and compares. Per-gate thresholds keep the easy gates strict.
    auto gate = [&](int g, const char* desc,
                    const QVector<GpuLayerInput>& gpuLayers, QSize canvas,
                    double ssimMin, double rgbMaeMax, double aMaeMax) {
        ++total;
        QVector<LayerDesc> descs;
        QVector<QImage> imgs;
        descs.reserve(gpuLayers.size());
        imgs.reserve(gpuLayers.size());
        for (const GpuLayerInput& in : gpuLayers) {
            descs.push_back(descFor(in));
            imgs.push_back(in.image);
        }
        const QImage cpu = hdrcomposite::compositeReference16(descs, imgs, canvas);
        const QImage gpuImg = gpu.composite16(gpuLayers, canvas);
        const Metrics16 m = compare16(cpu, gpuImg);
        const bool ok = m.valid && m.ssim >= ssimMin
                        && m.rgb16Mae <= rgbMaeMax && m.a16Mae <= aMaeMax;
        std::printf("[hdr-composite-parity] %s G%d %s (SSIM=%.5f >= %.3f, rgb16Mae=%.3f <= %.1f, a16Mae=%.3f <= %.1f)\n",
                    ok ? "PASS" : "FAIL", g, desc,
                    m.ssim, ssimMin, m.rgb16Mae, rgbMaeMax,
                    m.a16Mae, aMaeMax);
        if (ok) ++passed;
    };

    const QSize canvas(320, 240);
    const QSize srcSz(320, 240);

    // ------------------------------------------------------------------
    // G1: single full-canvas opaque layer, identity. Near-exact 16-bit match.
    // ------------------------------------------------------------------
    {
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(gradient16(srcSz, 20, 40, 60),
                              /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        // Tuned from a real-GL run (observed rgb16Mae=0.000, SSIM=1.0); small caps
        // retain failure power while tolerating driver rounding variance.
        gate(1, "single full-canvas opaque layer (identity)", g, canvas,
             0.999, 4.0, 4.0);
    }

    // ------------------------------------------------------------------
    // G2: two opaque full-canvas layers, V1-wins stacking. The frontmost
    // (lower sourceTrack) opaque layer wins every pixel in BOTH pipelines.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient16(srcSz, 200, 10, 10);   // backmost
        const QImage overlay = gradient16(srcSz, 10, 200, 10);   // frontmost
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        gate(2, "2 opaque full-canvas layers (V1-wins stacking)", g, canvas,
             0.999, 8.0, 8.0);
    }

    // ------------------------------------------------------------------
    // G3: upper layer at opacity 0.5 blended over an opaque base. Exercises
    // the 16-bit premultiplied blend (opacity scales all premul channels).
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient16(srcSz, 30, 30, 200);
        const QImage overlay = gradient16(srcSz, 220, 220, 20);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 0.5, 1.0, 0.0, 0.0, 0.0));
        gate(3, "opacity 0.5 overlay over opaque base (BLEND)", g, canvas,
             0.995, 8.0, 8.0);
    }

    // ------------------------------------------------------------------
    // G4: 16-BIT PRECISION PROOF (NON-TAUTOLOGICAL, no CPU compare).
    // A 600px-wide gradient with red = 20000 + x — ONE 16-bit step per column,
    // far below one 8-bit step (1/255 == 257 16-bit steps). Opaque, identity
    // composite16. Count DISTINCT red levels in the GPU output: a genuine
    // RGBA16 FBO preserves > 256 distinct levels, while the 8-bit projection
    // (red/257) of the SAME data collapses to <= 256. An RGBA8 FBO would have
    // collapsed the gradient to <= 256 on readback. This is the gate that
    // distinguishes HDR-2 from the 8-bit composite() path — it proves the FBO,
    // the glReadPixels(GL_UNSIGNED_SHORT) readback, and the RGBA64 image build
    // are all genuinely 16-bit end-to-end.
    // ------------------------------------------------------------------
    {
        ++total;
        const int W = 600, H = 1;
        QImage grad(W, H, QImage::Format_RGBA64);
        for (int x = 0; x < W; ++x) {
            QColor c;
            const quint16 r = static_cast<quint16>(20000 + x);
            c.setRgba64(qRgba64(r, 0, 0, 65535));
            grad.setPixelColor(x, 0, c);
        }
        const QSize gradCanvas(W, H);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(grad, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        const QImage out = gpu.composite16(g, gradCanvas);

        std::set<quint16> levels16;
        std::set<int> levels8;
        bool fmtOk = !out.isNull() && out.size() == gradCanvas;
        if (fmtOk) {
            for (int x = 0; x < W; ++x) {
                const quint16 r = out.pixelColor(x, 0).rgba64().red();
                levels16.insert(r);
                levels8.insert(r / 257);  // what an 8-bit path would have kept
            }
        }
        const bool ok = fmtOk && levels16.size() > 256 && levels8.size() <= 256;
        std::printf("[hdr-composite-parity] %s G4 16-bit precision proof "
                    "(distinct16=%zu > 256, distinct8proj=%zu <= 256)\n",
                    ok ? "PASS" : "FAIL", levels16.size(), levels8.size());
        if (ok) ++passed;
    }

    // ------------------------------------------------------------------
    // G5: PinP — upper layer scaled to 0.5 (centered) over an opaque base.
    // Nearest-neighbour sampling; edge rasterization deltas allowed.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient16(srcSz, 60, 90, 30);
        const QImage overlay = gradient16(srcSz, 240, 30, 120);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 0.5, 0.0, 0.0, 0.0));
        // Nearest-sampling boundary deltas are inherent (observed rgb16Mae≈344,
        // SSIM≈0.988); cap at 500 keeps ~1.45x headroom yet a real placement bug
        // (whole-texel shift across a gradient) would blow well past it and tank SSIM.
        gate(5, "PinP scaled-0.5 overlay over opaque base (nearest)", g, canvas,
             0.98, 500.0, 16.0);
    }

    // ------------------------------------------------------------------
    // G6: offset (videoDx/Dy) scaled-0.5 overlay placement. The shifted,
    // scaled overlay must land at the same canvas position in both pipelines.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient16(srcSz, 100, 20, 160);
        const QImage overlay = gradient16(srcSz, 20, 160, 100);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0,  0.0,  0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 0.5, 0.2, -0.15, 0.0));
        gate(6, "offset (videoDx/Dy) scaled-0.5 overlay placement", g, canvas,
             0.98, 500.0, 16.0);
    }

    // ------------------------------------------------------------------
    // G7: non-square 1280x720 canvas, two opaque full-canvas layers. Verifies
    // the Y-down ortho / transform preserves aspect on a wide canvas.
    // ------------------------------------------------------------------
    {
        const QSize wideCanvas(1280, 720);
        const QSize wideSrc(1280, 720);
        const QImage base    = gradient16(wideSrc, 180, 60, 20);
        const QImage overlay = gradient16(wideSrc, 20, 60, 180);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        gate(7, "non-square 1280x720 canvas (aspect preserved)", g, wideCanvas,
             0.999, 8.0, 8.0);
    }

    // ------------------------------------------------------------------
    // G8: translucent layers over a TRANSPARENT canvas. Exercises 16-bit ALPHA
    // readback correctness end-to-end. Semi-transparent solids mean a
    // premultiplied/straight-alpha mistake in upload or readback cannot hide
    // behind opaque pixels. Both layers are full-canvas; paintOrder puts the
    // lower-track layer frontmost. The CPU oracle composites the same stack
    // over a transparent RGBA64 canvas.
    // ------------------------------------------------------------------
    {
        const QSize alphaCanvas(96, 64);
        const QImage v2Back  = solid16(alphaCanvas, 200, 30, 20, 128);
        const QImage v1Front = solid16(alphaCanvas, 20, 40, 220, 128);
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(v1Front, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0)); // frontmost
        g.push_back(makeLayer(v2Back,  /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0)); // backmost
        // After the oracle premul fix (raw RGBA64 read/write), GPU and CPU agree
        // exactly here (observed rgb16Mae=0.000); tight caps lock that parity in.
        gate(8, "translucent layers over transparent canvas (16-bit alpha)",
             g, alphaCanvas, 0.995, 8.0, 8.0);
    }

    std::printf("[hdr-composite-parity] Result: %d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}
