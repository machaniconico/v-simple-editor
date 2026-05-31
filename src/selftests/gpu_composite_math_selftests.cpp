// gpu_composite_math_selftests.cpp
// Headless selftest for the pure gpucomposite:: compositing-math engine
// (src/playback/GpuCompositeMath.{h,cpp}).
// No QApplication and no runtime GUI dependencies (math types only). This file
// includes MaskSystem.h only for compile-time MatteType ordinal drift guards.
// Run via: --selftest=gpu-composite-math
//
// Verification strategy: every gate compares the engine output against a
// HAND-DERIVED oracle (computed independently below), never against the
// engine's own output — so a regression in the engine cannot make a gate
// pass tautologically.
//
// Matte byte-algebra note: G16..G24 are CPU-algebra reference-oracle gates
// mirroring MaskSystem's integer matte math. They are intentionally retained
// for drift detection, but gpucomposite::matteMaskValue/applyMaskPremul are
// NOT called by the GPU render path. The actual GLSL matte shader uses
// normalized floats and continuous luma; GPU-vs-CPU shader parity is gated by
// gpu-composite-parity (notably luma gates G11/G12).
//
// Coordinate note: gpucomposite::layerTransform maps native SOURCE-PIXEL coords
// onto CANVAS-PIXEL coords. It mirrors ClipGeometry::renderLayer by first
// scaling the native source to canvas dimensions, then un-centering by
// -canvas/2 and applying scale/rotation/anchor placement. For the identity
// case (scale=1, dx=dy=rot=0), any non-empty source rectangle maps onto the
// matching canvas rectangle. We verify source-quad points against
// hand-computed canvas positions.
//
// Gate map (24 gates):
//   G1  paintOrder: descending sourceTrack ordering
//   G2  layerTransform: identity (full-canvas) maps corners onto canvas corners
//   G3  layerTransform: videoScale=0.5 halves the quad about its center
//   G4  layerTransform: videoDx=0.25 shifts center by +0.25*canvasWidth
//   G5  layerTransform: rotation=90deg rotates corner by expected signed amount
//   G6  isLayerComposited: visible/empty/opacity guards
//   G7  clampOpacity: -0.5->0, 1.5->1, 0.3->0.3
//   G8  premulSourceOver: opaque red over anything -> red
//   G9  premulSourceOver: transparent src over dst -> dst unchanged
//   G10 premulSourceOver: half-alpha premul src over opaque white -> hand calc
//   G11 isValidMatteSource: (0,N) false, (-1,N) false, (1,3) true, (3,3) false
//   G12 paintOrder is a permutation (same count, no dupes, full coverage)
//   G13 paintOrder stability: equal sourceTrack keeps original relative order
//   G14 layerTransform: videoDy=-0.5 shifts center up by half canvas height
//   G15 layerTransform: srcSize!=canvas, non-square canvas, rotation preserves
//       ClipGeometry's source->canvas pre-scale semantics
//   G16 CPU-oracle matteMaskValue Alpha -> premulAlpha
//   G17 CPU-oracle matteMaskValue AlphaInverted -> 255-premulAlpha
//   G18 CPU-oracle matteMaskValue Luma R=G=B=10 -> 10 (Rec.601 int)
//   G19 CPU-oracle matteMaskValue Luma R=1,G=B=0 truncates 0.299->0
//   G20 CPU-oracle matteMaskValue LumaInverted R=G=B=10 -> 245
//   G21 CPU-oracle matteMaskValue None -> 255
//   G22 CPU-oracle applyMaskPremul truncation (r=100,g=50,b=0,a=200,mask=128)
//   G23 3-arg isValidMatteSource valid case (2,0,4) -> true
//   G24 3-arg isValidMatteSource rejects idx0 / self-ref / OOB / negative

#include <cstdio>
#include <cmath>

#include <QSize>
#include <QVector>
#include <QVector3D>
#include <QMatrix4x4>

#include "../playback/GpuCompositeMath.h"
#include "../MaskSystem.h"

using gpucomposite::LayerDesc;
using gpucomposite::RGBAf;
using gpucomposite::MatteType;

static_assert(static_cast<int>(MatteType::None)
              == static_cast<int>(TrackMatteType::None),
              "gpucomposite::MatteType::None must match TrackMatteType::None");
static_assert(static_cast<int>(MatteType::Alpha)
              == static_cast<int>(TrackMatteType::AlphaMatte),
              "gpucomposite::MatteType::Alpha must match TrackMatteType::AlphaMatte");
static_assert(static_cast<int>(MatteType::AlphaInverted)
              == static_cast<int>(TrackMatteType::AlphaInvertedMatte),
              "gpucomposite::MatteType::AlphaInverted must match TrackMatteType::AlphaInvertedMatte");
static_assert(static_cast<int>(MatteType::Luminance)
              == static_cast<int>(TrackMatteType::LumaMatte),
              "gpucomposite::MatteType::Luminance must match TrackMatteType::LumaMatte");
static_assert(static_cast<int>(MatteType::LuminanceInverted)
              == static_cast<int>(TrackMatteType::LumaInvertedMatte),
              "gpucomposite::MatteType::LuminanceInverted must match TrackMatteType::LumaInvertedMatte");

namespace {

bool nearEqualF(double a, double b, double eps = 1e-3)
{
    return std::fabs(a - b) < eps;
}

// Map a source-pixel point (x,y) through a layerTransform matrix to canvas px.
QVector3D mapPoint(const QMatrix4x4& m, double x, double y)
{
    return m.map(QVector3D(static_cast<float>(x), static_cast<float>(y), 0.0f));
}

bool ptNear(const QVector3D& p, double ex, double ey, double eps = 1e-3)
{
    return nearEqualF(p.x(), ex, eps) && nearEqualF(p.y(), ey, eps);
}

} // anonymous namespace

int runGpuCompositeMathSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[gpu-composite-math] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    // ------------------------------------------------------------------
    // G1: paintOrder descending by sourceTrack.
    // Input sourceTracks {0,2,1} (indices 0,1,2). Descending track order is
    // 2 (idx1), 1 (idx2), 0 (idx0) -> oracle index sequence {1,2,0}.
    // ------------------------------------------------------------------
    {
        QVector<LayerDesc> layers;
        LayerDesc a; a.sourceTrack = 0; layers.push_back(a); // idx0
        LayerDesc b; b.sourceTrack = 2; layers.push_back(b); // idx1
        LayerDesc c; c.sourceTrack = 1; layers.push_back(c); // idx2
        const QVector<int> order = gpucomposite::paintOrder(layers);
        const QVector<int> oracle = { 1, 2, 0 };
        check(1, "paintOrder descending sourceTrack", order == oracle);
    }

    // ------------------------------------------------------------------
    // G2: identity transform. srcSize==canvas (1920x1080), scale1, dx/dy0, rot0.
    // Source corners (0,0),(1920,0),(0,1080),(1920,1080) map onto the same
    // canvas corners. Hand oracle: anchor=(960,540), un-center=(-960,-540).
    //   (0,0)        -> (0,0)
    //   (1920,1080)  -> (1920,1080)
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(1920, 1080);
        const QSize canvas(1920, 1080);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        const bool tl = ptNear(mapPoint(m, 0, 0),        0,    0);
        const bool tr = ptNear(mapPoint(m, 1920, 0),     1920, 0);
        const bool bl = ptNear(mapPoint(m, 0, 1080),     0,    1080);
        const bool br = ptNear(mapPoint(m, 1920, 1080),  1920, 1080);
        check(2, "identity maps source corners to canvas corners",
              tl && tr && bl && br);
    }

    // ------------------------------------------------------------------
    // G3: videoScale=0.5 about layer center. canvas 1000x1000, src 1000x1000.
    // anchor=(500,500). For a source point P, output = anchor + 0.5*(P-center),
    // center=(500,500). Hand oracle:
    //   src center (500,500) -> (500,500)  (fixed point)
    //   src (0,0)            -> 500 + 0.5*(0-500) = 250 -> (250,250)
    //   src (1000,1000)      -> 500 + 0.5*(500)  = 750 -> (750,750)
    // i.e. a 1000-wide quad becomes 500-wide, centered.
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(1000, 1000); l.videoScale = 0.5;
        const QSize canvas(1000, 1000);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        const bool ctr = ptNear(mapPoint(m, 500, 500), 500, 500);
        const bool tl  = ptNear(mapPoint(m, 0, 0),     250, 250);
        const bool br  = ptNear(mapPoint(m, 1000, 1000), 750, 750);
        check(3, "videoScale=0.5 halves quad about center", ctr && tl && br);
    }

    // ------------------------------------------------------------------
    // G4: videoDx=0.25 on canvas 1000x1000, src 1000x1000, scale1.
    // anchorX = 1000*0.5 + 0.25*1000 = 750. The layer center maps to (750,500).
    // (un-center: src center 500 -> 0 -> +anchor 750).
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(1000, 1000); l.videoDx = 0.25;
        const QSize canvas(1000, 1000);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        // Center of source -> anchor.
        const bool ctr = ptNear(mapPoint(m, 500, 500), 750, 500);
        // The whole quad shifts by +250 in x; left corner: 0 -> 250.
        const bool left = ptNear(mapPoint(m, 0, 500), 250, 500);
        check(4, "videoDx=0.25 shifts center by +0.25*canvasWidth", ctr && left);
    }

    // ------------------------------------------------------------------
    // G5: rotation2DDegrees=90 about layer center. canvas/src 1000x1000.
    // Op order: translate(anchor) * rotate(90,Z) * scale(1) * translate(-500,-500).
    // Take src point (1000,500) (right-mid). After un-center: (500,0).
    // QMatrix4x4::rotate uses a right-handed rotation about +Z; for a Y-down
    // raster the visual effect is a CW turn. Compute numerically the same way
    // the engine does, but build the oracle from a SEPARATE QTransform-free
    // hand rotation matrix to avoid reusing the engine.
    //   R(90) about +Z (Qt convention): x' = x*cos - y*sin, y' = x*sin + y*cos
    //   cos90=0, sin90=1 -> (500,0) -> (0*500-1*0, 1*500+0*0) = (0,500)
    //   + anchor(500,500) -> (500,1000).
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(1000, 1000); l.rotation2DDegrees = 90.0;
        const QSize canvas(1000, 1000);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        // Hand oracle for src (1000,500): un-center (500,0), rotate +90 about Z,
        // re-anchor. cos90=0 sin90=1: (x',y') = (x*0 - y*1, x*1 + y*0) = (0,500).
        const double cx = 500, cy = 500;       // anchor
        const double ux = 1000 - 500, uy = 500 - 500; // un-centered (500,0)
        const double rx = ux * 0.0 - uy * 1.0; // 0
        const double ry = ux * 1.0 + uy * 0.0; // 500
        const double ox = cx + rx;             // 500
        const double oy = cy + ry;             // 1000
        const bool ok = ptNear(mapPoint(m, 1000, 500), ox, oy, 1e-2);
        check(5, "rotation=90 rotates corner by signed Qt convention", ok);
    }

    // ------------------------------------------------------------------
    // G6: isLayerComposited guards.
    // ------------------------------------------------------------------
    {
        LayerDesc good; good.visible = true; good.srcSize = QSize(10, 10); good.opacity = 1.0;
        LayerDesc invis = good; invis.visible = false;
        LayerDesc empty = good; empty.srcSize = QSize();
        LayerDesc faint = good; faint.opacity = 0.0005;
        const bool ok = gpucomposite::isLayerComposited(good)
                     && !gpucomposite::isLayerComposited(invis)
                     && !gpucomposite::isLayerComposited(empty)
                     && !gpucomposite::isLayerComposited(faint);
        check(6, "isLayerComposited visible/size/opacity guards", ok);
    }

    // ------------------------------------------------------------------
    // G7: clampOpacity.
    // ------------------------------------------------------------------
    {
        const bool ok = nearEqualF(gpucomposite::clampOpacity(-0.5), 0.0)
                     && nearEqualF(gpucomposite::clampOpacity(1.5), 1.0)
                     && nearEqualF(gpucomposite::clampOpacity(0.3), 0.3);
        check(7, "clampOpacity bounds -0.5->0,1.5->1,0.3->0.3", ok);
    }

    // ------------------------------------------------------------------
    // G8: opaque red src over any dst -> red (src.a=1 => out = src).
    // out.rgb = src.rgb + dst.rgb*(1-1)=src.rgb; out.a=1+dst.a*0=1.
    // ------------------------------------------------------------------
    {
        RGBAf src{1.0f, 0.0f, 0.0f, 1.0f};
        RGBAf dst{0.2f, 0.7f, 0.9f, 0.4f};
        RGBAf out = gpucomposite::premulSourceOver(src, dst);
        const bool ok = nearEqualF(out.r, 1.0) && nearEqualF(out.g, 0.0)
                     && nearEqualF(out.b, 0.0) && nearEqualF(out.a, 1.0);
        check(8, "opaque red over anything -> red", ok);
    }

    // ------------------------------------------------------------------
    // G9: fully transparent src (0,0,0,0) over dst -> dst unchanged.
    // out = src.rgb(0) + dst.rgb*(1-0) = dst; out.a = 0 + dst.a*1 = dst.a.
    // ------------------------------------------------------------------
    {
        RGBAf src{0.0f, 0.0f, 0.0f, 0.0f};
        RGBAf dst{0.2f, 0.3f, 0.4f, 0.5f};
        RGBAf out = gpucomposite::premulSourceOver(src, dst);
        const bool ok = nearEqualF(out.r, 0.2) && nearEqualF(out.g, 0.3)
                     && nearEqualF(out.b, 0.4) && nearEqualF(out.a, 0.5);
        check(9, "transparent src over dst -> dst unchanged", ok);
    }

    // ------------------------------------------------------------------
    // G10: half-alpha premul src (0.5,0,0,0.5) over opaque white (1,1,1,1).
    // inv = 1-0.5 = 0.5.
    //   out.r = 0.5 + 1*0.5 = 1.0
    //   out.g = 0.0 + 1*0.5 = 0.5
    //   out.b = 0.0 + 1*0.5 = 0.5
    //   out.a = 0.5 + 1*0.5 = 1.0
    // ------------------------------------------------------------------
    {
        RGBAf src{0.5f, 0.0f, 0.0f, 0.5f};
        RGBAf dst{1.0f, 1.0f, 1.0f, 1.0f};
        RGBAf out = gpucomposite::premulSourceOver(src, dst);
        const bool ok = nearEqualF(out.r, 1.0) && nearEqualF(out.g, 0.5)
                     && nearEqualF(out.b, 0.5) && nearEqualF(out.a, 1.0);
        check(10, "half-alpha premul src over white -> hand calc", ok);
    }

    // ------------------------------------------------------------------
    // G11: isValidMatteSource.
    // ------------------------------------------------------------------
    {
        const bool ok = !gpucomposite::isValidMatteSource(0, 5)   // idx 0 reserved
                     && !gpucomposite::isValidMatteSource(-1, 5)  // negative
                     &&  gpucomposite::isValidMatteSource(1, 3)   // valid
                     && !gpucomposite::isValidMatteSource(3, 3);  // out of range
        check(11, "isValidMatteSource (0,N)F (-1,N)F (1,3)T (3,3)F", ok);
    }

    // ------------------------------------------------------------------
    // G12: paintOrder returns a permutation: same count, no dupes, full
    // coverage of [0,count). Use scrambled tracks {5,1,5,9,0}.
    // ------------------------------------------------------------------
    {
        QVector<LayerDesc> layers;
        const int tracks[] = {5, 1, 5, 9, 0};
        for (int t : tracks) { LayerDesc d; d.sourceTrack = t; layers.push_back(d); }
        const QVector<int> order = gpucomposite::paintOrder(layers);
        bool ok = (order.size() == layers.size());
        QVector<bool> seen(layers.size(), false);
        for (int idx : order) {
            if (idx < 0 || idx >= layers.size() || seen[idx]) { ok = false; break; }
            seen[idx] = true;
        }
        for (bool s : seen) if (!s) ok = false;
        check(12, "paintOrder is a permutation (count/unique/coverage)", ok);
    }

    // ------------------------------------------------------------------
    // G13: paintOrder stability for equal sourceTrack.
    // Three layers all on track 7 (idx 0,1,2). Stable descending sort must
    // preserve original relative order -> oracle {0,1,2}.
    // ------------------------------------------------------------------
    {
        QVector<LayerDesc> layers;
        for (int i = 0; i < 3; ++i) { LayerDesc d; d.sourceTrack = 7; layers.push_back(d); }
        const QVector<int> order = gpucomposite::paintOrder(layers);
        const QVector<int> oracle = { 0, 1, 2 };
        check(13, "paintOrder stable on equal sourceTrack", order == oracle);
    }

    // ------------------------------------------------------------------
    // G14: videoDy=-0.5 on canvas 1000x1000, src 1000x1000, scale1.
    // anchorY = 1000*0.5 + (-0.5)*1000 = 0. Layer center maps to (500,0).
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(1000, 1000); l.videoDy = -0.5;
        const QSize canvas(1000, 1000);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        const bool ctr = ptNear(mapPoint(m, 500, 500), 500, 0);
        check(14, "videoDy=-0.5 shifts center up by half canvas height", ctr);
    }

    // ------------------------------------------------------------------
    // G15: ClipGeometry parity for native source != canvas with rotation.
    // src 200x100, canvas 400x300, rotation +90. renderLayer first pre-scales
    // src to canvas (sx=2, sy=3), then resolveTransform un-centers by
    // -canvas/2 and rotates. Hand oracle for two source points:
    //
    // right-mid (200,50):
    //   pre-scale -> (400,150), un-center -> (200,0)
    //   R90 -> (0,200), anchor(200,150) -> (200,350)
    //
    // top-mid (100,0):
    //   pre-scale -> (200,0), un-center -> (0,-150)
    //   R90 -> (150,0), anchor(200,150) -> (350,150)
    //
    // The old native-size un-center form would have produced (200,250) and
    // (250,150), proving center anchoring alone is not enough for parity.
    // ------------------------------------------------------------------
    {
        LayerDesc l; l.srcSize = QSize(200, 100); l.rotation2DDegrees = 90.0;
        const QSize canvas(400, 300);
        const QMatrix4x4 m = gpucomposite::layerTransform(l, canvas);
        const bool rightMid = ptNear(mapPoint(m, 200, 50), 200, 350, 1e-2);
        const bool topMid   = ptNear(mapPoint(m, 100, 0),  350, 150, 1e-2);
        check(15, "src!=canvas non-square + rotation matches ClipGeometry pre-scale",
              rightMid && topMid);
    }

    // ------------------------------------------------------------------
    // G16: CPU-oracle matteMaskValue Alpha -> premulAlpha (no luma calculation).
    // premulAlpha=200, any RGB irrelevant. Oracle: 200.
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::Alpha, 100, 150, 200, /*premulAlpha=*/200);
        check(16, "matteMaskValue Alpha -> premulAlpha", mask == 200);
    }

    // ------------------------------------------------------------------
    // G17: CPU-oracle matteMaskValue AlphaInverted -> 255 - premulAlpha.
    // premulAlpha=200. Oracle: 255-200 = 55.
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::AlphaInverted, 100, 150, 200, /*premulAlpha=*/200);
        check(17, "matteMaskValue AlphaInverted -> 255-premulAlpha", mask == 55);
    }

    // ------------------------------------------------------------------
    // G18: CPU-oracle matteMaskValue Luma uniform grey (no fractional part).
    // R=10, G=10, B=10.
    // 0.299*10 + 0.587*10 + 0.114*10 = 2.99 + 5.87 + 1.14 = 10.00 -> 10.
    // Oracle: 10.
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::Luminance, 10, 10, 10, /*premulAlpha=*/255);
        check(18, "matteMaskValue Luma R=G=B=10 -> 10 (Rec.601 truncate)", mask == 10);
    }

    // ------------------------------------------------------------------
    // G19: CPU-oracle matteMaskValue Luma fractional-truncation case.
    // This documents MaskSystem's integer truncation. The GPU shader's luma
    // path is continuous float and is covered by gpu-composite-parity instead.
    // R=1, G=0, B=0. 0.299*1 + 0.587*0 + 0.114*0 = 0.299 -> truncate -> 0.
    // Oracle: 0. (NOT 1 — must NOT round.)
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::Luminance, 1, 0, 0, /*premulAlpha=*/255);
        check(19, "matteMaskValue Luma R=1 G=B=0 truncates 0.299->0", mask == 0);
    }

    // ------------------------------------------------------------------
    // G20: CPU-oracle matteMaskValue LumaInverted.
    // R=10, G=10, B=10 -> luma=10 -> 255-10 = 245. Oracle: 245.
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::LuminanceInverted, 10, 10, 10, /*premulAlpha=*/255);
        check(20, "matteMaskValue LumaInverted R=G=B=10 -> 245", mask == 245);
    }

    // ------------------------------------------------------------------
    // G21: CPU-oracle matteMaskValue None -> 255 (full opacity, no matte).
    // ------------------------------------------------------------------
    {
        const int mask = gpucomposite::matteMaskValue(
            MatteType::None, 50, 100, 200, /*premulAlpha=*/128);
        check(21, "matteMaskValue None -> 255", mask == 255);
    }

    // ------------------------------------------------------------------
    // G22: CPU-oracle applyMaskPremul integer-truncation.
    // Input: r=100, g=50, b=0, a=200. maskVal=128.
    // Oracle (integer truncation, NOT rounding):
    //   r = 100*128/255 = 12800/255 = 50  (50.196... truncates to 50)
    //   g =  50*128/255 =  6400/255 = 25  (25.098... truncates to 25)
    //   b =   0*128/255 = 0
    //   a = 200*128/255 = 25600/255 = 100 (100.392... truncates to 100)
    // ------------------------------------------------------------------
    {
        int r = 100, g = 50, b = 0, a = 200;
        gpucomposite::applyMaskPremul(r, g, b, a, /*maskVal=*/128);
        check(22, "applyMaskPremul integer truncation r/g/b/a",
              r == 50 && g == 25 && b == 0 && a == 100);
    }

    // ------------------------------------------------------------------
    // G23: 3-arg isValidMatteSource valid case.
    // matteIdx=2, layerIdx=0, count=4 -> 2>0 && 2<4 && 2!=0 -> true.
    // ------------------------------------------------------------------
    {
        const bool ok = gpucomposite::isValidMatteSource(2, 0, 4);
        check(23, "3-arg isValidMatteSource valid (2,0,4) -> true", ok);
    }

    // ------------------------------------------------------------------
    // G24: 3-arg isValidMatteSource rejection cases.
    //   matteIdx=0            -> false (reserved V1 slot)
    //   matteIdx==layerIdx=2  -> false (layer cannot matte itself)
    //   matteIdx>=count       -> false (out of range)
    //   matteIdx=-1           -> false (negative)
    // ------------------------------------------------------------------
    {
        const bool ok =
            !gpucomposite::isValidMatteSource(0, 1, 4)   // reserved index 0
         && !gpucomposite::isValidMatteSource(2, 2, 4)   // self-reference
         && !gpucomposite::isValidMatteSource(4, 0, 4)   // out of range (==count)
         && !gpucomposite::isValidMatteSource(-1, 0, 4); // negative
        check(24, "3-arg isValidMatteSource rejects idx0/self/OOB/negative", ok);
    }

    std::printf("[gpu-composite-math] Result: %d/%d PASSED\n",
                passed, passed + failed);
    return failed; // 0 = all PASS
}
