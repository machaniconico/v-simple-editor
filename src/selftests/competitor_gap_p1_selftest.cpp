// Wave 27 P1 competitor-gap features selftest (8 gates):
//   G1: Subclips CRUD + Timeline insert + 1-step Undo + SubclipManagerDialog
//   G2: TimelineMinimap dual-timeline overview snapshot + coordinate mapping + Dialog
//   G3: Film Halation warm highlight bleed + alpha preservation + disabled no-op
//   G4: Posterize Time quantization + SSOT TimelineFrameRenderer::renderFrameAt step parity
//   G5: 3D Camera DOF CoC radius math + bokeh blur + SSOT renderFrameAt depth blur
//   G6: Speed Graph |dv/dt| EaseInOut velocity bell curve + SpeedGraphDialog
//   G7: Shy Layers filter persistence + UI list hiding + always-rendered guarantee
//   G8: CreativeEffectsDialog round-trip + fast-path zero-overhead restoration

#include "../AeProTools.h"
#include "../CreativeEffects.h"
#include "../ExternalGenerator.h"
#include "../ShapeLayer.h"
#include "../SubclipStore.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TimelineMinimap.h"
#include "SelftestGate.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {

double meanAbsPixelDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 1e9;
    QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    const int w = ia.width();
    const int h = ia.height();
    for (int y = 0; y < h; ++y) {
        const uchar *la = ia.constScanLine(y);
        const uchar *lb = ib.constScanLine(y);
        for (int x = 0; x < w * 4; ++x)
            sum += std::abs(int(la[x]) - int(lb[x]));
    }
    return sum / double(w * h * 4);
}

double meanHorizontalEdgeContrast(const QImage &img)
{
    if (img.isNull() || img.width() < 2 || img.height() < 1)
        return 0.0;
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    int count = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 1; x < rgba.width(); ++x) {
            sum += std::abs(int(line[x * 4]) - int(line[(x - 1) * 4]));
            ++count;
        }
    }
    return count > 0 ? (sum / count) : 0.0;
}

QImage makeCheckerImage(int w, int h, int cell)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool white = (((x / cell) + (y / cell)) % 2) == 0;
            const uchar v = white ? 240 : 20;
            line[x * 4 + 0] = v;
            line[x * 4 + 1] = v;
            line[x * 4 + 2] = v;
            line[x * 4 + 3] = 255;
        }
    }
    return img;
}

Shape makeSolidRectShape(const QPointF &center, const QSizeF &size, const QColor &color)
{
    Shape s;
    s.type = ShapeType::Rectangle;
    s.position = center;
    s.properties.size = size;
    s.fill.enabled = true;
    s.fill.color = color;
    s.stroke.enabled = false;
    return s;
}

} // namespace

int runCompetitorGapP1Selftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        rec(1, false, QStringLiteral("QTemporaryDir unavailable"));
        return tally.finish("competitor-gap-p1");
    }

    // Ensure clean starting state for global stores.
    creative::clearAllConfigsForTest();
    aepro::clearShyState();

    // ------------------------------------------------------------------
    // G1: Subclips CRUD + Timeline Insert + 1-Step Undo + Dialog
    // ------------------------------------------------------------------
    {
        const QString jsonPath = tmpDir.filePath(QStringLiteral("subclips.json"));
        ClipInfo src;
        src.filePath = QStringLiteral("C:/media/interview_take1.mp4");
        src.displayName = QStringLiteral("Interview Take 1");
        src.duration = 20.0;
        src.inPoint = 1.5;
        src.outPoint = 4.5;

        const auto created = subclips::createSubclipFromClip(src, QStringLiteral("Best Quote"), jsonPath);
        const auto loaded = subclips::loadSubclips(jsonPath);

        Timeline tl;
        const bool inserted = subclips::insertSubclipIntoTimeline(&tl, created, 0);
        const bool hasClip = !tl.videoTracks().isEmpty() && tl.videoTracks()[0]->clips().size() == 1;
        const bool trimMatches = hasClip
            && std::abs(tl.videoTracks()[0]->clips()[0].inPoint - 1.5) < 1e-6
            && std::abs(tl.videoTracks()[0]->clips()[0].outPoint - 4.5) < 1e-6;

        subclips::SubclipManagerDialog dlg(&tl, nullptr, jsonPath);
        const bool dialogCountOk = (dlg.subclipCount() == 1);

        tl.undo();
        const bool undoneOk = tl.videoTracks()[0]->clips().isEmpty();

        const bool removed = subclips::removeSubclip(created.id, jsonPath);
        const bool emptyAfterRemove = subclips::loadSubclips(jsonPath).isEmpty();

        const bool ok = !created.id.isEmpty()
            && std::abs(created.duration() - 3.0) < 1e-6
            && loaded.size() == 1
            && inserted && trimMatches && dialogCountOk && undoneOk
            && removed && emptyAfterRemove;
        rec(1, ok, QStringLiteral("dur=%1 loaded=%2 ins=%3 trim=%4 dlg=%5 undo=%6 rem=%7")
                       .arg(created.duration(), 0, 'f', 2).arg(loaded.size())
                       .arg(inserted ? 1 : 0).arg(trimMatches ? 1 : 0)
                       .arg(dialogCountOk ? 1 : 0).arg(undoneOk ? 1 : 0)
                       .arg((removed && emptyAfterRemove) ? 1 : 0));
    }

    // ------------------------------------------------------------------
    // G2: TimelineMinimap Overview Snapshot + Coordinate Mapping + Dialog
    // ------------------------------------------------------------------
    {
        Timeline tl;
        ClipInfo c1;
        c1.filePath = QStringLiteral("v1a.mp4");
        c1.duration = 4.0;
        c1.inPoint = 0.0;
        c1.outPoint = 4.0;
        ClipInfo c2 = c1;
        c2.filePath = QStringLiteral("v1b.mp4");
        c2.duration = 5.0;
        c2.outPoint = 5.0;
        tl.videoTracks()[0]->addClip(c1);
        tl.videoTracks()[0]->addClip(c2);

        ClipInfo a1;
        a1.filePath = QStringLiteral("a1.wav");
        a1.duration = 8.0;
        a1.inPoint = 0.0;
        a1.outPoint = 8.0;
        tl.audioTracks()[0]->addClip(a1);
        tl.addMarker(3'000'000, QStringLiteral("Beat"));

        const auto snap = minimap::buildOverviewSnapshot(&tl);
        const double x = minimap::timelineSecToX(4.25, 400.0, 10.0);
        const double roundTrip = minimap::xToTimelineSec(x, 400.0, 10.0);

        minimap::TimelineMinimapDialog dlg(&tl);
        dlg.minimap()->setViewportRange(1.0, 5.0);
        const bool ok = (snap.bars.size() == 3)
            && (snap.totalDurationSec >= 8.99)
            && snap.markerTimesSec.contains(3.0)
            && std::abs(roundTrip - 4.25) < 1e-6
            && dlg.minimap()->snapshot().bars.size() == 3
            && std::abs(dlg.minimap()->viewportStartSec() - 1.0) < 1e-6
            && std::abs(dlg.minimap()->viewportEndSec() - 5.0) < 1e-6;
        rec(2, ok, QStringLiteral("bars=%1 dur=%2 mk=%3 rt=%4 vp=[%5,%6]")
                       .arg(snap.bars.size()).arg(snap.totalDurationSec, 0, 'f', 2)
                       .arg(snap.markerTimesSec.size()).arg(roundTrip, 0, 'f', 2)
                       .arg(dlg.minimap()->viewportStartSec(), 0, 'f', 1)
                       .arg(dlg.minimap()->viewportEndSec(), 0, 'f', 1));
    }

    // ------------------------------------------------------------------
    // G3: Film Halation Warm Highlight Bleed + Alpha Preservation + No-Op
    // ------------------------------------------------------------------
    {
        QImage src(64, 64, QImage::Format_RGBA8888);
        src.fill(QColor(0, 0, 0, 0));
        // Dark opaque interior + bright white core
        for (int y = 8; y < 56; ++y) {
            uchar *line = src.scanLine(y);
            for (int x = 8; x < 56; ++x) {
                const bool bright = (x >= 26 && x < 38 && y >= 26 && y < 38);
                line[x * 4 + 0] = bright ? 255 : 20;
                line[x * 4 + 1] = bright ? 255 : 20;
                line[x * 4 + 2] = bright ? 255 : 20;
                line[x * 4 + 3] = 255;
            }
        }

        creative::HalationParams off;
        off.enabled = false;
        const QImage noop = creative::applyFilmHalation(src, off);
        const bool noopExact = (meanAbsPixelDiff(src, noop) == 0.0);

        creative::HalationParams on;
        on.enabled = true;
        on.threshold = 0.75;
        on.radiusPx = 8.0;
        on.strength = 0.95;
        const QImage out = creative::applyFilmHalation(src, on);

        // Pixel just outside bright core (23, 32) should show warm red-orange halo (R > G > B)
        const uchar *haloPx = out.constScanLine(32) + 23 * 4;
        const bool warmBleed = (haloPx[0] > 40) && (haloPx[0] > haloPx[1]) && (haloPx[1] > haloPx[2]);
        // Transparent outer pixel (2, 2) must keep alpha == 0
        const uchar *borderPx = out.constScanLine(2) + 2 * 4;
        const bool alphaPreserved = (borderPx[3] == 0);

        rec(3, noopExact && warmBleed && alphaPreserved,
            QStringLiteral("noop=%1 halo=(%2,%3,%4) borderA=%5")
                .arg(noopExact ? 1 : 0).arg(haloPx[0]).arg(haloPx[1]).arg(haloPx[2]).arg(borderPx[3]));
    }

    // ------------------------------------------------------------------
    // G4: Posterize Time Quantization + SSOT renderFrameAt Step Parity
    // ------------------------------------------------------------------
    {
        creative::PosterizeTimeParams pt;
        pt.enabled = true;
        pt.targetFps = 4.0;
        const bool mathOk =
            std::abs(creative::quantizePosterizeTime(0.31, pt) - 0.25) < 1e-6
            && std::abs(creative::quantizePosterizeTime(0.49, pt) - 0.25) < 1e-6
            && std::abs(creative::quantizePosterizeTime(0.51, pt) - 0.50) < 1e-6;

        // Real SSOT render test using built-in animated AudioVisualizer generator
        Timeline tl;
        ClipInfo clip;
        clip.uid = QStringLiteral("w27-posterize-clip");
        clip.filePath = QStringLiteral("veditor://extgen/builtin/audio-visualizer?title=PT");
        clip.displayName = QStringLiteral("PT Gen");
        clip.duration = 4.0;
        clip.inPoint = 0.0;
        clip.outPoint = 4.0;
        tl.videoTracks()[0]->addClip(clip);

        const QSize sz(160, 90);
        // Without posterize: 0.10s and 0.20s should differ
        const QImage rawA = tlrender::renderFrameAt(&tl, 100'000, sz);
        const QImage rawB = tlrender::renderFrameAt(&tl, 200'000, sz);
        const double rawDiff = meanAbsPixelDiff(rawA, rawB);
        const bool rawVaries = rawDiff > 0.1;

        // With posterize at 2 fps (0.5s steps): 0.10s and 0.20s both quantize to 0.0s!
        creative::ClipCreativeConfig cfg;
        cfg.posterizeTime.enabled = true;
        cfg.posterizeTime.targetFps = 2.0;
        creative::setConfigForClip(clip.uid, cfg);

        const QImage stepA = tlrender::renderFrameAt(&tl, 100'000, sz);
        const QImage stepB = tlrender::renderFrameAt(&tl, 200'000, sz);
        const QImage stepC = tlrender::renderFrameAt(&tl, 600'000, sz);
        const double sameDiff = meanAbsPixelDiff(stepA, stepB);
        const double nextDiff = meanAbsPixelDiff(stepA, stepC);
        const bool sameWithinStep = sameDiff == 0.0;
        const bool differsNextStep = nextDiff > 0.1;

        creative::clearConfigForClip(clip.uid);
        rec(4, mathOk && rawVaries && sameWithinStep && differsNextStep,
            QStringLiteral("math=%1 rawDiff=%2 sameDiff=%3 nextDiff=%4")
                .arg(mathOk ? 1 : 0).arg(rawDiff, 0, 'f', 2)
                .arg(sameDiff, 0, 'f', 2).arg(nextDiff, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G5: 3D Camera DOF CoC Math + Bokeh Blur + SSOT renderFrameAt
    // ------------------------------------------------------------------
    {
        creative::CameraDofParams dof;
        dof.enabled = true;
        dof.focusDistance = 1000.0;
        dof.aperture = 80.0;
        dof.maxBlurPx = 18.0;

        const double rFocus = creative::computeDofBlurRadiusPx(1000.0, dof);
        const double rFar = creative::computeDofBlurRadiusPx(2000.0, dof);
        const bool cocOk = (rFocus == 0.0) && (rFar > 6.0);

        const QImage checker = makeCheckerImage(64, 64, 4);
        const QImage blurred = creative::applyCameraDofBlur(checker, 6.0);
        const double sharpContrast = meanHorizontalEdgeContrast(checker);
        const double blurContrast = meanHorizontalEdgeContrast(blurred);
        const bool bokehSoftens = (blurContrast < sharpContrast * 0.55);

        // SSOT renderFrameAt: high-contrast 3D shape layer stays sharp at focusDistance==1000
        // and blurs cleanly when focus plane shifts far away (zero perspective-scale confound).
        Timeline tl;
        ClipInfo clip;
        clip.uid = QStringLiteral("w27-dof-clip");
        clip.displayName = QStringLiteral("3D Layer");
        clip.duration = 2.0;
        clip.inPoint = 0.0;
        clip.outPoint = 2.0;
        clip.is3DLayer = true;
        clip.layer3D.positionZ = 0.0; // distance 1000 == focusDistance
        clip.shapes.append(makeSolidRectShape(QPointF(32.0, 32.0), QSizeF(64.0, 64.0), QColor(20, 20, 20, 255)));
        for (int i = 0; i < 4; ++i) {
            clip.shapes.append(makeSolidRectShape(
                QPointF(8.0 + i * 16.0, 32.0), QSizeF(6.0, 64.0), QColor(240, 240, 240, 255)));
        }
        tl.videoTracks()[0]->addClip(clip);

        creative::ClipCreativeConfig cfg;
        cfg.cameraDof = dof;
        creative::setConfigForClip(clip.uid, cfg);

        const QImage inFocusFrame = tlrender::renderFrameAt(&tl, 500'000, QSize(64, 64));

        // Shift focus plane far from the layer (distance=1000 vs focus=2600 -> strong bokeh)
        cfg.cameraDof.focusDistance = 2600.0;
        creative::setConfigForClip(clip.uid, cfg);

        const QImage outFocusFrame = tlrender::renderFrameAt(&tl, 500'000, QSize(64, 64));
        const double cIn = meanHorizontalEdgeContrast(inFocusFrame);
        const double cOut = meanHorizontalEdgeContrast(outFocusFrame);
        const bool ssotDofOk = !inFocusFrame.isNull() && !outFocusFrame.isNull()
            && (cIn > 15.0) && (cOut < cIn * 0.6);

        creative::clearConfigForClip(clip.uid);
        rec(5, cocOk && bokehSoftens && ssotDofOk,
            QStringLiteral("rFoc=%1 rFar=%2 sharp=%3 blur=%4 cIn=%5 cOut=%6")
                .arg(rFocus, 0, 'f', 1).arg(rFar, 0, 'f', 1)
                .arg(sharpContrast, 0, 'f', 1).arg(blurContrast, 0, 'f', 1)
                .arg(cIn, 0, 'f', 1).arg(cOut, 0, 'f', 1));
    }

    // ------------------------------------------------------------------
    // G6: Speed Graph |dv/dt| EaseInOut Velocity Curve + Dialog
    // ------------------------------------------------------------------
    {
        KeyframeTrack tr(QStringLiteral("positionX"), 0.0);
        tr.addKeyframe(0.0, 0.0, KeyframePoint::EaseInOut);
        tr.addKeyframe(2.0, 200.0, KeyframePoint::EaseInOut);

        const auto sum = aepro::sampleSpeedCurve(tr, 0.0, 2.0, 65);
        const bool peakMid = std::abs(sum.peakTimeSec - 1.0) < 0.25;
        const bool bellShape = (sum.maxSpeed > sum.avgSpeed * 1.2)
            && (sum.samples.first().speed < sum.maxSpeed * 0.25)
            && (sum.samples.last().speed < sum.maxSpeed * 0.25);

        KeyframeManager mgr;
        mgr.addTrack(tr);
        aepro::SpeedGraphDialog dlg(mgr, QStringLiteral("MotionClip"), 2.0);
        const bool dlgOk = (dlg.trackCount() == 1) && (dlg.currentSummary().maxSpeed > 100.0);

        rec(6, peakMid && bellShape && dlgOk,
            QStringLiteral("peakT=%1 max=%2 avg=%3 dlgCnt=%4")
                .arg(sum.peakTimeSec, 0, 'f', 2).arg(sum.maxSpeed, 0, 'f', 1)
                .arg(sum.avgSpeed, 0, 'f', 1).arg(dlg.trackCount()));
    }

    // ------------------------------------------------------------------
    // G7: Shy Layers Filter + Always-Rendered Guarantee
    // ------------------------------------------------------------------
    {
        aepro::clearShyState();

        Timeline tl;
        tl.addVideoTrack(); // V1 (0) and V2 (1)
        ClipInfo clip;
        clip.displayName = QStringLiteral("Shy Red Layer");
        clip.duration = 2.0;
        clip.inPoint = 0.0;
        clip.outPoint = 2.0;
        clip.shapes.append(makeSolidRectShape(QPointF(32.0, 32.0), QSizeF(64.0, 64.0), QColor(220, 40, 40, 255)));
        tl.videoTracks()[0]->addClip(clip);

        aepro::setTrackShy(false, 0, true);
        aepro::setShyFilterEnabled(true);

        const QVector<int> vis = aepro::visibleTrackIndices(2, false);
        const bool uiFiltered = (vis.size() == 1 && vis[0] == 1);

        // AE invariant: shy tracks STILL render into preview & export!
        const QImage rendered = tlrender::renderFrameAt(&tl, 500'000, QSize(64, 64));
        const QColor center = rendered.isNull() ? QColor() : rendered.pixelColor(32, 32);
        const bool stillRendered = !rendered.isNull() && (center.red() > 180 && center.green() < 80);

        aepro::ShyLayersDialog dlg(&tl);
        const bool dlgOk = (dlg.visibleVideoTrackCount() == 1);

        aepro::clearShyState();
        rec(7, uiFiltered && stillRendered && dlgOk,
            QStringLiteral("vis=%1 rgb=(%2,%3,%4) dlgVis=%5")
                .arg(vis.size()).arg(center.red()).arg(center.green()).arg(center.blue())
                .arg(dlg.visibleVideoTrackCount()));
    }

    // ------------------------------------------------------------------
    // G8: CreativeEffectsDialog Round-Trip + Fast-Path Restoration
    // ------------------------------------------------------------------
    {
        const QString uid = QStringLiteral("w27-dlg-roundtrip");
        creative::ClipCreativeConfig cfg;
        cfg.halation.enabled = true;
        cfg.halation.threshold = 0.68;
        cfg.halation.radiusPx = 15.0;
        cfg.halation.strength = 0.85;
        cfg.posterizeTime.enabled = true;
        cfg.posterizeTime.targetFps = 8.0;
        cfg.cameraDof.enabled = true;
        cfg.cameraDof.focusDistance = 1400.0;
        cfg.cameraDof.aperture = 75.0;
        cfg.cameraDof.maxBlurPx = 22.0;
        creative::setConfigForClip(uid, cfg);

        creative::CreativeEffectsDialog dlg(uid, QStringLiteral("TestClip"));
        const auto readBack = dlg.currentConfig();
        const bool matches =
            readBack.halation.enabled
            && std::abs(readBack.halation.threshold - 0.68) < 1e-3
            && std::abs(readBack.halation.radiusPx - 15.0) < 1e-3
            && std::abs(readBack.halation.strength - 0.85) < 1e-3
            && readBack.posterizeTime.enabled
            && std::abs(readBack.posterizeTime.targetFps - 8.0) < 1e-3
            && readBack.cameraDof.enabled
            && std::abs(readBack.cameraDof.focusDistance - 1400.0) < 1e-3
            && std::abs(readBack.cameraDof.aperture - 75.0) < 1e-3
            && std::abs(readBack.cameraDof.maxBlurPx - 22.0) < 1e-3;

        creative::clearAllConfigsForTest();
        const bool fastPathRestored = !creative::hasAnyActiveConfig();

        rec(8, matches && fastPathRestored,
            QStringLiteral("matches=%1 fastPath=%2")
                .arg(matches ? 1 : 0).arg(fastPathRestored ? 1 : 0));
    }

    return tally.finish("competitor-gap-p1");
}
