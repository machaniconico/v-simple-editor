// src/selftests/gpu_composite_parity_selftests.cpp
// GPU vs CPU SSOT measured-parity selftest for the Stage 2 GpuLayerCompositor.
//
// Stage 1 (gpu-composite-math) proved the PURE compositing math headless.
// Stage 2 (this test) drives the REAL GPU pipeline (GpuLayerCompositor::composite
// -> offscreen FBO + premultiplied source-over + read-back) against the project's
// CPU compositing SSOT (VideoPlayer::composeMultiTrackFrameForTest, which forwards
// into the genuine private composeMultiTrackFrame). For matte-FREE scenes the two
// must agree within tight perceptual tolerances.
//
// Inputs are mapped between the two APIs as follows:
//   * CPU SSOT: composeMultiTrackFrameForTest(v1Frame, overlayRgb[], opacity[],
//     scale[], dx[], dy[], rotation[]) paints v1Frame first and each overlay ON
//     TOP of it in vector order (last overlay frontmost).
//   * GPU: composite(layers, canvas) paints in gpucomposite::paintOrder, which is
//     DESCENDING sourceTrack (lowest sourceTrack drawn LAST == frontmost, V1-wins).
//   G1..G7 feed both APIs equivalent visible stacks. G8 additionally exercises
//   the production sourceTrack mapping directly: a transparent CPU canvas plus
//   layers already in production paint order (V2 back, V1 front) is compared
//   against GPU layers carrying the real sourceTrack values.
//
// Matte support: GpuLayerCompositor draws matte-requesting layers as plain layers
// this stage (full GPU matte is the next stage), so matte parity is intentionally
// NOT gated here. Matte-free scenes are the Stage 2 guarantee and are exercised by
// G1..G8 below.
//
// Graceful skip: if no usable GL context exists (headless box / WSL without GL),
// GpuLayerCompositor::isAvailable() returns false (or composite() returns a null
// image). In that case we print an explicit SKIP line and return 0 — never a
// silent pass and never a FAIL (see reference_selftest_wsl_windows_env). The test
// must not hang.

#include "../playback/GpuLayerCompositor.h"
#include "../playback/GpuCompositeMath.h"
#include "../VideoPlayer.h"

#include <QApplication>
#include <QGuiApplication>
#include <QImage>
#include <QColor>
#include <QSize>
#include <QVector>
#include <QString>

#include <cstdio>
#include <cmath>

namespace {

using gpucomposite::LayerDesc;

// A solid RGBA test image of the given native size.
QImage solid(QSize sz, QColor c)
{
    QImage img(sz, QImage::Format_ARGB32);
    img.fill(c);
    return img;
}

QImage transparentCanvas(QSize sz)
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// A deterministic gradient test image so sampling/placement differences show up
// (a flat fill would hide many transform bugs).
QImage gradient(QSize sz, int rBase, int gBase, int bBase)
{
    QImage img(sz, QImage::Format_ARGB32);
    for (int y = 0; y < sz.height(); ++y) {
        for (int x = 0; x < sz.width(); ++x) {
            const int r = (rBase + x * 3) & 0xFF;
            const int g = (gBase + y * 3) & 0xFF;
            const int b = (bBase + (x + y) * 2) & 0xFF;
            img.setPixel(x, y, qRgba(r, g, b, 255));
        }
    }
    return img;
}

// Mean absolute error (per channel, 0..255 scale) + SSIM (single-window global
// luma SSIM) between two images. Computed on ARGB32 so byte layout is fixed.
// Returns false if the images are not comparable (size/null mismatch).
struct Metrics {
    double mae = 1e9;       // mean absolute error over RGB channels, 0..255
    double alphaMae = 1e9;  // mean absolute error over alpha, 0..255
    double ssim = 0.0;      // global luma SSIM, -1..1
    bool   valid = false;
};

Metrics compare(const QImage& aIn, const QImage& bIn)
{
    Metrics out;
    if (aIn.isNull() || bIn.isNull())
        return out;
    if (aIn.size() != bIn.size())
        return out;

    const QImage a = aIn.convertToFormat(QImage::Format_ARGB32);
    const QImage b = bIn.convertToFormat(QImage::Format_ARGB32);

    const int w = a.width();
    const int h = a.height();
    const double n = double(w) * double(h);
    if (n <= 0.0)
        return out;

    double absSum = 0.0;          // RGB absolute diff
    double alphaAbsSum = 0.0;
    // Luma stats for SSIM.
    double meanA = 0.0, meanB = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pa = ra[x];
            const QRgb pb = rb[x];
            absSum += std::abs(qRed(pa)   - qRed(pb));
            absSum += std::abs(qGreen(pa) - qGreen(pb));
            absSum += std::abs(qBlue(pa)  - qBlue(pb));
            alphaAbsSum += std::abs(qAlpha(pa) - qAlpha(pb));
            const double la = 0.299 * qRed(pa) + 0.587 * qGreen(pa) + 0.114 * qBlue(pa);
            const double lb = 0.299 * qRed(pb) + 0.587 * qGreen(pb) + 0.114 * qBlue(pb);
            meanA += la;
            meanB += lb;
        }
    }
    out.mae = absSum / (n * 3.0);
    out.alphaMae = alphaAbsSum / n;
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

    // SSIM constants for an 8-bit dynamic range (L=255).
    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    out.ssim = ((2.0 * meanA * meanB + C1) * (2.0 * cov + C2)) /
               ((meanA * meanA + meanB * meanB + C1) * (varA + varB + C2));
    out.valid = true;
    return out;
}

// Build a GpuLayerInput from primitives. sourceTrack controls GPU paint order.
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
    in.desc.matteSourceIndex = -1;
    in.desc.matteType = gpucomposite::MatteType::None;
    return in;
}

} // anonymous namespace

int runGpuCompositeParitySelftest()
{
    // Need a Q(Gui)Application for the GL context. The registry dispatches this
    // entry AFTER QApplication is constructed (needsQApplication=true), so an
    // instance should already exist; we never construct one here.
    if (!qApp) {
        std::printf("[gpu-composite-parity] SKIP: no QApplication instance (GL unavailable)\n");
        return 0;
    }

    GpuLayerCompositor gpu;
    if (!gpu.isAvailable()) {
        std::printf("[gpu-composite-parity] SKIP: no usable GL context (headless/WSL)\n");
        return 0;
    }

    // Probe: a trivial single-layer composite. If the GPU path returns an empty
    // image even though isAvailable() said yes, treat that as "no usable GL"
    // and SKIP rather than FAIL.
    const QSize probeCanvas(64, 48);
    {
        QVector<GpuLayerInput> probe;
        probe.push_back(makeLayer(solid(probeCanvas, QColor(120, 130, 140, 255)),
                                  /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        const QImage probeImg = gpu.composite(probe, probeCanvas);
        if (probeImg.isNull() || probeImg.size() != probeCanvas) {
            std::printf("[gpu-composite-parity] SKIP: GPU composite returned empty (headless/WSL)\n");
            return 0;
        }
    }

    VideoPlayer player; // QWidget; safe to construct under QApplication, not shown.

    int passed = 0;
    int total = 0;

    // gate(): run one comparison. cpu = CPU SSOT result, gpuLayers/canvas drive
    // the GPU path. thresholds are per-gate so the easy gates stay strict.
    auto gate = [&](int g, const char* desc,
                    const QImage& cpu,
                    const QVector<GpuLayerInput>& gpuLayers, QSize canvas,
                    double ssimMin, double maeMax, double alphaMaeMax) {
        ++total;
        const QImage gpuImg = gpu.composite(gpuLayers, canvas);
        const Metrics m = compare(cpu, gpuImg);
        const bool ok = m.valid && m.ssim >= ssimMin
                        && m.mae <= maeMax && m.alphaMae <= alphaMaeMax;
        std::printf("[gpu-composite-parity] %s G%d %s (SSIM=%.5f >= %.3f, RGB_MAE=%.4f <= %.3f, A_MAE=%.4f <= %.3f)\n",
                    ok ? "PASS" : "FAIL", g, desc,
                    m.ssim, ssimMin, m.mae, maeMax,
                    m.alphaMae, alphaMaeMax);
        if (ok) ++passed;
    };

    const QSize canvas(320, 240);
    const QSize srcSz(320, 240);

    // ------------------------------------------------------------------
    // G1: single full-canvas layer, scale 1, opacity 1. Near-exact match.
    // CPU: v1Frame == the single layer, no overlays.
    // GPU: one layer, full canvas.
    // ------------------------------------------------------------------
    {
        const QImage base = gradient(srcSz, 20, 40, 60);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, {}, {}, {}, {}, {}, {});
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        gate(1, "single full-canvas layer", cpu, g, canvas, 0.98, 1.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G2: two opaque full-canvas layers, V1-wins. CPU paints base then the
    // overlay on top (overlay frontmost). To make GPU reproduce that exact
    // visible stacking, the overlay gets the LOWER sourceTrack (frontmost),
    // and the base gets the HIGHER sourceTrack (backmost).
    // Since both are fully opaque + full canvas, the frontmost layer wins
    // every pixel in BOTH pipelines, so they must agree.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient(srcSz, 200, 10, 10);   // V1-ish base
        const QImage overlay = gradient(srcSz, 10, 200, 10);   // on top
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { overlay }, { 1.0 }, { 1.0 }, { 0.0 }, { 0.0 }, { 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0)); // backmost
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0)); // frontmost
        gate(2, "2 opaque full-canvas layers (V1-wins stacking)", cpu, g, canvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G3: upper layer at opacity 0.5 blended over an opaque base.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient(srcSz, 30, 30, 200);
        const QImage overlay = gradient(srcSz, 220, 220, 20);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { overlay }, { 0.5 }, { 1.0 }, { 0.0 }, { 0.0 }, { 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 0.5, 1.0, 0.0, 0.0, 0.0));
        gate(3, "opacity 0.5 upper-layer blend", cpu, g, canvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G4: PinP — upper layer scaled to 0.5 (centered). Edge sampling around
    // the scaled rect differs slightly between QPainter nearest and GL nearest,
    // so RGB_MAE allows small rasterization deltas; SSIM must still hold.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient(srcSz, 60, 90, 30);
        const QImage overlay = gradient(srcSz, 240, 30, 120);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { overlay }, { 1.0 }, { 0.5 }, { 0.0 }, { 0.0 }, { 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 0.5, 0.0, 0.0, 0.0));
        gate(4, "PinP scaled-0.5 upper layer", cpu, g, canvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G5: offset layer (videoDx/Dy). The scaled-0.5 overlay is shifted by a
    // normalized canvas offset; the placement must match between pipelines.
    // ------------------------------------------------------------------
    {
        const QImage base    = gradient(srcSz, 100, 20, 160);
        const QImage overlay = gradient(srcSz, 20, 160, 100);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { overlay }, { 1.0 }, { 0.5 }, { 0.2 }, { -0.15 }, { 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0,  0.0,   0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 0.5, 0.2, -0.15,  0.0));
        gate(5, "offset (videoDx/Dy) scaled layer placement", cpu, g, canvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G6: three visible-stack layers. Two opaque scaled
    // overlays stacked over an opaque base, distinct positions.
    // CPU vector order: [V2, V3] painted over base, V3 last (frontmost).
    // GPU tracks: base=2 (backmost), V2=1, V3=0 (frontmost) -> same stack.
    // ------------------------------------------------------------------
    {
        const QImage base = gradient(srcSz, 40, 40, 40);
        const QImage v2   = gradient(srcSz, 210, 50, 50);
        const QImage v3   = gradient(srcSz, 50, 50, 210);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { v2, v3 }, { 1.0, 1.0 }, { 0.5, 0.4 },
            { -0.2, 0.2 }, { -0.2, 0.2 }, { 0.0, 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base, /*track*/2, 1.0, 1.0,  0.0,  0.0, 0.0)); // backmost
        g.push_back(makeLayer(v2,   /*track*/1, 1.0, 0.5, -0.2, -0.2, 0.0));
        g.push_back(makeLayer(v3,   /*track*/0, 1.0, 0.4,  0.2,  0.2, 0.0)); // frontmost
        gate(6, "3-layer (V1+V2+V3) composite", cpu, g, canvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G7: non-square canvas (1280x720) version of G2. Verifies the transform
    // does not break the aspect ratio (the real-draw counterpart of the
    // Stage-1 G15 non-square matrix gate). Two opaque full-canvas layers.
    // ------------------------------------------------------------------
    {
        const QSize wideCanvas(1280, 720);
        const QSize wideSrc(1280, 720);
        const QImage base    = gradient(wideSrc, 180, 60, 20);
        const QImage overlay = gradient(wideSrc, 20, 60, 180);
        const QImage cpu = player.composeMultiTrackFrameForTest(
            base, { overlay }, { 1.0 }, { 1.0 }, { 0.0 }, { 0.0 }, { 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(base,    /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0));
        g.push_back(makeLayer(overlay, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0));
        gate(7, "non-square 1280x720 canvas (aspect preserved)", cpu, g, wideCanvas, 0.98, 3.0, 1.0);
    }

    // ------------------------------------------------------------------
    // G8: production sourceTrack order with translucent layers over a
    // transparent canvas. CPU receives the already-sorted production paint
    // order (V2 back, V1 front); GPU receives real sourceTrack values in the
    // opposite vector order so paintOrder itself must put V1 frontmost.
    // This also gates read-back alpha and prevents premultiplied/straight
    // alpha mistakes from hiding behind fully opaque test images.
    // ------------------------------------------------------------------
    {
        const QSize alphaCanvas(96, 64);
        const QImage v2Back  = solid(alphaCanvas, QColor(200, 30, 20, 128));
        const QImage v1Front = solid(alphaCanvas, QColor(20, 40, 220, 128));
        const QImage cpu = player.composeMultiTrackFrameForTest(
            transparentCanvas(alphaCanvas),
            { v2Back, v1Front }, { 1.0, 1.0 }, { 1.0, 1.0 },
            { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 });
        QVector<GpuLayerInput> g;
        g.push_back(makeLayer(v1Front, /*track*/0, 1.0, 1.0, 0.0, 0.0, 0.0)); // frontmost V1
        g.push_back(makeLayer(v2Back,  /*track*/1, 1.0, 1.0, 0.0, 0.0, 0.0)); // backmost V2
        gate(8, "production sourceTrack order + translucent alpha", cpu, g,
             alphaCanvas, 0.98, 2.0, 2.0);
    }

    // NOTE: matte-bearing scenes (LayerDesc::matteType != None) are NOT gated
    // here. GpuLayerCompositor draws matte layers as plain layers this stage;
    // full GPU matte (alpha/luminance/inverted sampled from the matte source
    // in the fragment shader) is the next stage's parity work.

    std::printf("[gpu-composite-parity] Result: %d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}
