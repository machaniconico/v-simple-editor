// US-708: MainWindow::buildSpecialClipComposite (special-data preview path) must
// run the same native FX stack (mesh warp / puppet pins / FX pack) as the export
// path tlrender::renderFrameAt, so preview and export agree.
#include "../GLPreview.h"
#include "../MainWindow.h"
#include "../RotoClipEntry.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../VideoPlayer.h"
#include "../TimelineFrameRenderer.h"
#include "../WarpDistortion.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QOpenGLContext>
#include <QRect>
#include <QTest>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int kFps = 10;

bool writeClip(const QString &path, bool varying = false)
{
    QImage frame(kW, kH, QImage::Format_RGB888);
    frame.fill(Qt::black);
    for (int y = 0; y < kH; ++y)
        for (int x = 222; x <= 226; ++x)
            frame.setPixelColor(x, y, Qt::white);

    libavcore::EncodeRequest request;
    request.width = kW;
    request.height = kH;
    request.fps = request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 2000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto error = encoder.open(request)) {
        std::cerr << "special-composite fixture open: " << *error << '\n';
        return false;
    }
    for (int i = 0; i < kFps; ++i) {
        if (varying)
            frame.fill(QColor(i * 25, 255 - i * 25, 60 + i * 10));
        if (!encoder.pushFrameRgb24(frame.constBits(), frame.bytesPerLine(), i)) {
            std::cerr << "special-composite fixture encode failed at " << i << '\n';
            return false;
        }
    }
    if (const auto error = encoder.finalize()) {
        std::cerr << "special-composite fixture finalize: " << *error << '\n';
        return false;
    }
    return true;
}

struct DiffStat {
    bool sizeMatch = false;
    int maxDiff = 255;
    qint64 diffPixels = -1;
};

DiffStat compare(const QImage &a, const QImage &b)
{
    DiffStat stat;
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return stat;
    stat.sizeMatch = true;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    stat.maxDiff = 0;
    stat.diffPixels = 0;
    for (int y = 0; y < ia.height(); ++y) {
        const uchar *pa = ia.constScanLine(y);
        const uchar *pb = ib.constScanLine(y);
        for (int x = 0; x < ia.width(); ++x) {
            int px = 0;
            for (int c = 0; c < 4; ++c)
                px = std::max(px, std::abs(int(pa[x * 4 + c]) - int(pb[x * 4 + c])));
            stat.maxDiff = std::max(stat.maxDiff, px);
            if (px > 2)
                ++stat.diffPixels;
        }
    }
    return stat;
}

// Largest RGB (alpha ignored) channel difference inside `rect`.
int rgbMaxDiff(const QImage &a, const QImage &b, const QRect &rect)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 255;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    int maxDiff = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *pa = ia.constScanLine(y);
        const uchar *pb = ib.constScanLine(y);
        for (int x = rect.left(); x <= rect.right(); ++x)
            for (int c = 0; c < 3; ++c)
                maxDiff = std::max(maxDiff, std::abs(int(pa[x * 4 + c]) - int(pb[x * 4 + c])));
    }
    return maxDiff;
}

// Largest per-channel distance of every pixel in `rect` from the GL clear
// colour (0.1 grey = 25.5, so 25 is used and 25/26 both land within 1).
int rgbMaxDiffFromClearGrey(const QImage &image, const QRect &rect)
{
    if (image.isNull())
        return 255;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    int maxDiff = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = rect.left(); x <= rect.right(); ++x)
            for (int c = 0; c < 3; ++c)
                maxDiff = std::max(maxDiff, std::abs(int(row[x * 4 + c]) - 25));
    }
    return maxDiff;
}

// One GL view's grabs: the widget is kW x kH logical, so grabFramebuffer must
// return exactly qRound(kW * dpr) x qRound(kH * dpr) device pixels. With the
// frame texture sampled Nearest, frame pixel (x, y) is the device pixel
// (floor((x + 0.5) * dpr), floor((y + 0.5) * dpr)) with no resampling.
struct DeviceGrab {
    qreal dpr = 0.0;
    QSize rawSize;
    bool rawSizeOk = true; // cleared by any grab of the wrong device size
    int grabs = 0;

    QImage toFrame(const QImage &raw, qreal widgetDpr)
    {
        ++grabs;
        dpr = widgetDpr;
        rawSize = raw.size();
        const QSize device(qRound(kW * dpr), qRound(kH * dpr));
        if (raw.isNull() || raw.size() != device) {
            rawSizeOk = false;
            std::cerr << "special-composite grab size mismatch raw=" << raw.width() << 'x' << raw.height()
                      << " expectedDevice=" << device.width() << 'x' << device.height()
                      << " dpr=" << dpr << '\n';
            if (raw.isNull())
                return QImage();
        }
        QImage frame(kW, kH, QImage::Format_RGB32);
        for (int y = 0; y < kH; ++y) {
            const int dy = std::min(raw.height() - 1, int(std::floor((y + 0.5) * dpr)));
            for (int x = 0; x < kW; ++x) {
                const int dx = std::min(raw.width() - 1, int(std::floor((x + 0.5) * dpr)));
                frame.setPixel(x, y, raw.pixel(dx, dy) | 0xff000000u);
            }
        }
        return frame;
    }
};

} // namespace

int runSpecialCompositeSelftest()
{
    selftest::GateTally tally;
    const QSize size(kW, kH);

    QString encoderWhy;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderWhy)) {
        std::cerr << "special-composite: " << encoderWhy.toStdString() << '\n';
        for (int g = 1; g <= 7; ++g)
            tally.skip(g, "mpeg4 encoder unavailable");
        return tally.finish("special-composite");
    }

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("special-composite.mp4"));
    const bool fixtureOk = dir.isValid() && writeClip(path);

    MainWindow window;
    Timeline *timeline = window.m_timeline;
    window.m_projectConfig.width = kW;
    window.m_projectConfig.height = kH;
    window.m_projectConfig.fps = kFps;

    ClipInfo base;
    base.filePath = path;
    base.duration = base.outPoint = 1.0;
    base.speed = 1.0;
    base.opacity = 1.0;

    const MeshGrid identity = WarpDistortion::createDefaultMesh(QSize(1, 1), 3, 3);
    MeshGrid moved = identity;
    for (auto &row : moved.controlPoints)
        row[1].rx() += 0.2;

    const double t = 0.5;
    const auto setClip = [&](const ClipInfo &clip) {
        timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    };

    ClipInfo remapped = base;
    remapped.timeRemapCurve.sourceFps = kFps;
    remapped.timeRemapCurve.addKey(0.0, 0.0);
    remapped.timeRemapCurve.addKey(1.0, 1.0);

    if (!fixtureOk || !timeline) {
        tally.gate(1, false, "fixture or timeline missing");
        tally.gate(2, false, "fixture or timeline missing");
        tally.gate(3, false, "fixture or timeline missing");
        tally.gate(4, false, "fixture or timeline missing");
        for (int g = 5; g <= 7; ++g)
            tally.gate(g, false, "fixture or timeline missing");
        return tally.finish("special-composite");
    }

    // G1: time-remap + mesh warp: preview == export, and the warp is visible.
    ClipInfo warped = remapped;
    warped.meshWarp = moved;
    setClip(warped);
    const QImage previewWarp = window.buildSpecialClipComposite(t);
    const QImage exportWarp = tlrender::renderFrameAt(
        timeline, qint64(t * 1000000.0), size);
    const DiffStat g1 = compare(previewWarp, exportWarp);

    // G2: same clip, warp off: preview == export (no unrelated difference).
    setClip(remapped);
    const QImage previewPlain = window.buildSpecialClipComposite(t);
    const QImage exportPlain = tlrender::renderFrameAt(
        timeline, qint64(t * 1000000.0), size);
    const DiffStat g2 = compare(previewPlain, exportPlain);
    const DiffStat warpVisible = compare(previewWarp, previewPlain);

    std::cerr << "special-composite G1 sizeMatch=" << g1.sizeMatch
              << " maxDiff=" << g1.maxDiff << " diffPixels=" << g1.diffPixels
              << " warpVsPlainMaxDiff=" << warpVisible.maxDiff
              << " warpVsPlainDiffPixels=" << warpVisible.diffPixels << '\n';
    std::cerr << "special-composite G2 sizeMatch=" << g2.sizeMatch
              << " maxDiff=" << g2.maxDiff << " diffPixels=" << g2.diffPixels << '\n';

    tally.gate(1, g1.sizeMatch && g1.maxDiff <= 2
                   && warpVisible.diffPixels > 100,
               "preview/export mismatch with mesh warp (see values above)");
    tally.gate(2, g2.sizeMatch && g2.maxDiff <= 2,
               "preview/export mismatch without mesh warp (see values above)");

    // G3: no special data (no remap) + mesh warp: special path is not entered.
    ClipInfo plainWarp = base;
    plainWarp.meshWarp = moved;
    setClip(plainWarp);
    const QImage skipped = window.buildSpecialClipComposite(t);
    std::cerr << "special-composite G3 isNull=" << skipped.isNull() << '\n';
    tally.gate(3, skipped.isNull(), "special path entered without special data");

    // G4: time remap running past the out point + Echo. Export clamps the
    // source second to the out point before Echo samples (delay 0.2s); the
    // preview must use the same clamped second. Frames change per index so a
    // different sampled second shows up as a pixel difference.
    const QString varyPath = dir.filePath(QStringLiteral("special-composite-vary.mp4"));
    const bool varyOk = writeClip(varyPath, true);
    ClipInfo echoClip = base;
    echoClip.filePath = varyPath;
    echoClip.timeRemapCurve.sourceFps = kFps;
    echoClip.timeRemapCurve.addKey(0.0, 0.0);
    echoClip.timeRemapCurve.addKey(0.5, 1.0);
    echoClip.outPoint = 0.5;
    echoClip.effects = { VideoEffect::createEcho(0.2, 1, 0.5, 0) };
    setClip(echoClip);
    const double t4 = 0.4;
    const double rawSrc = echoClip.timeRemapCurve.srcTimeAt(t4);
    const double clampedSrc = qBound(echoClip.inPoint, echoClip.inPoint + rawSrc,
                                     echoClip.outPoint);
    const QImage previewEcho = window.buildSpecialClipComposite(t4);
    const QImage exportEcho = tlrender::renderFrameAt(
        timeline, qint64(t4 * 1000000.0), size);
    const DiffStat g4 = compare(previewEcho, exportEcho);
    std::cerr << "special-composite G4 previewNull=" << previewEcho.isNull() << " exportNull=" << exportEcho.isNull() << "\n";
    std::cerr << "special-composite G4 sizeMatch=" << g4.sizeMatch
              << " maxDiff=" << g4.maxDiff << " diffPixels=" << g4.diffPixels
              << " rawRemapSourceSec=" << rawSrc
              << " clampedSourceSec(export)=" << clampedSrc << '\n';
    tally.gate(4, varyOk && g4.sizeMatch && g4.maxDiff <= 2,
               "preview/export mismatch for out-of-range remap + Echo (see values above)");

    // G5-G7 (US-1106): GLPreview output of a special-composite frame. The
    // widget is the frame's logical size with no display transform and the
    // frame texture sampled Nearest, so each frame pixel is read back exactly
    // from its device pixel (DeviceGrab). Compared on RGB only.
    VideoPlayer player;
    GLPreview *gl = player.glPreview();
    bool glOk = false;
    if (gl) {
        gl->setFixedSize(kW, kH);
        gl->setFrameTextureNearestForTest(true);
        player.show();
        for (int i = 0; i < 8 && !glOk; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            const QImage probe = gl->grabFramebuffer();
            glOk = gl->context() && gl->context()->isValid() && !probe.isNull();
        }
    }
    if (!glOk) {
        for (int g = 5; g <= 7; ++g)
            tally.skip(g, "no usable QOpenGLWidget context");
        return tally.finish("special-composite");
    }
    // grabFramebuffer returns device pixels (dpr x the logical kW x kH). The
    // frame texture is sampled Nearest, so each frame pixel is read back from
    // its device pixel exactly; a grab of any other size fails G5-G7.
    DeviceGrab glGrab;
    const auto grab = [&]() {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        return glGrab.toFrame(gl->grabFramebuffer(), gl->devicePixelRatioF());
    };

    // Roto rectangle on a non-default grade: buildSpecialClipComposite output
    // already holds the grade (renderFrameAt applies it), so GL must not grade
    // again.
    ColorCorrection grade;
    grade.brightness = 30.0;
    grade.contrast = 25.0;
    grade.saturation = -40.0;
    ClipInfo gradedClip = base;
    gradedClip.filePath = varyPath;
    gradedClip.colorCorrection = grade;
    setClip(gradedClip);
    const QString rotoKey = trackMatteClipKey(0, 0);
    RotoClipEntry rotoEntry;
    rotoEntry.clipId = rotoKey;
    RotoPoint corner;
    corner.position = corner.handleIn = corner.handleOut = QPointF();
    for (const QPointF &p : {QPointF(80, 45), QPointF(240, 45), QPointF(240, 135), QPointF(80, 135)}) {
        corner.position = corner.handleIn = corner.handleOut = p;
        rotoEntry.path.points.append(corner);
    }
    rotoEntry.path.closed = true;
    timeline->setRotoEntries(QHash<QString, RotoClipEntry>{{rotoKey, rotoEntry}});

    const double t5 = 0.5;
    const QImage special = window.buildSpecialClipComposite(t5);
    const QImage expected = tlrender::renderFrameAt(timeline, qint64(t5 * 1000000.0), size);
    // Sanity: the same roto without the grade differs inside, so a missing or
    // doubled grade cannot hide behind a flat fixture.
    ClipInfo ungradedClip = gradedClip;
    ungradedClip.colorCorrection = ColorCorrection{};
    setClip(ungradedClip);
    timeline->setRotoEntries(QHash<QString, RotoClipEntry>{{rotoKey, rotoEntry}});
    const QImage ungraded = window.buildSpecialClipComposite(t5);
    const QRect inner(86, 51, 148, 78);
    const QRect outer(10, 10, kW - 20, 25);
    const int gradeVisible = rgbMaxDiff(special, ungraded, inner);

    // G6 first (the negative control needs GL effects still enabled): the
    // pre-US-1106 path, grade set on the player and the frame handed straight
    // to GLPreview, grades the already graded frame a second time.
    player.setColorCorrection(grade);
    gl->displayFrame(special);
    const QImage headGrab = grab();
    const int g6Inner = rgbMaxDiff(headGrab, expected, inner);

    // G5 (real MainWindow routes): the paused VideoPlayer::displayFrame (which
    // pulls the special composite from MainWindow's provider) and the two
    // displaySpecialComposite callers, driven on the window's own player and
    // GL view.
    VideoPlayer *wp = window.m_player;
    GLPreview *wgl = wp ? wp->glPreview() : nullptr;
    bool routesOk = false;
    bool sentinelsOk = false;
    int routeFrameInner = 255, routeRefreshInner = 255, routeStillInner = 255;
    int routeFrameOuter = 255, routeRefreshOuter = 255, routeStillOuter = 255;
    bool routeFrameBaked = false, routeRefreshBaked = false, routeStillBaked = false;
    bool restoreBakedOk = false, restoreImageOk = false, restoreFxOk = false;
    bool seekOk = false;
    int seekStaleDiff = 0, seekZeroDiff = 0, seekInner = 255, seekOuter = 255, stopInner = 255, stopOuter = 255;
    bool seekBaked = false, stopBaked = false;
    int aidOuterClear = 255;
    QImage::Format aidUploadFormat = QImage::Format_Invalid;
    int textBakeInner = 255, textLinearInner = 0, textBakeVisible = 0, textExpectedVisible = 0, aidVisible = 0, aidRestored = 255, textStrokePx = 0;
    bool texelCentred = false;
    DeviceGrab wglGrab;
    if (wp && wgl) {
        wgl->setFixedSize(kW, kH);
        wgl->setFrameTextureNearestForTest(true);
        window.show();
        bool wOk = false;
        for (int i = 0; i < 8 && !wOk; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            const QImage probe = wgl->grabFramebuffer();
            wOk = wgl->context() && wgl->context()->isValid() && !probe.isNull();
        }
        const auto wgrab = [&]() {
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            return wglGrab.toFrame(wgl->grabFramebuffer(), wgl->devicePixelRatioF());
        };
        const auto setGradedClip = [&](bool withRoto) {
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{gradedClip}},
                QVector<QVector<ClipInfo>>{}, t5, -1.0, -1.0, 100);
            timeline->setRotoEntries(withRoto
                ? QHash<QString, RotoClipEntry>{{rotoKey, rotoEntry}}
                : QHash<QString, RotoClipEntry>{});
        };
        setGradedClip(false);
        wp->pause();
        wp->loadFile(varyPath);
        // Ordinary paused display path: a seek away and back re-runs
        // VideoPlayer::displayFrame.
        // One seek loads the source frame; later refreshes use the public,
        // synchronous setTextOverlays({}) -> refreshDisplayedFrame() ->
        // displayFrame (the ordinary paused path).
        {
            bool seeked = false;
            const auto conn = QObject::connect(wp, &VideoPlayer::positionChanged, wp,
                [&](double pos) { if (std::abs(pos - t5) < 0.001) seeked = true; });
            wp->seek(int(t5 * 1000));
            (void)QTest::qWaitFor([&] { return seeked; }, 15000);
            QObject::disconnect(conn);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        const auto ordinaryRefresh = [&]() { wp->setTextOverlays({}); };
        // Ordinary (no roto) paused refresh: the state a non-special clip gets.
        ordinaryRefresh();
        const QImage plainGrab = wgrab();
        const bool baseBaked = wgl->compositeBakedMode();
        const bool baseFx = wgl->effectsEnabled();

        setGradedClip(true);
        // Routes 2 and 3 must replace a known-wrong frame themselves: the
        // ordinary refresh already leaves the special frame on screen via the
        // provider, so un-bake and show an opaque magenta sentinel
        // first and record that it is neither the expected inner colour nor
        // the clear grey.
        struct PreState { int inner; int outer; bool baked; };
        const auto sentinelPre = [&]() {
            wgl->setCompositeBakedMode(false);
            QImage sentinel(kW, kH, QImage::Format_ARGB32_Premultiplied);
            sentinel.fill(qRgb(255, 0, 255));
            wgl->displayFrame(sentinel);
            const QImage g = wgrab();
            return PreState{rgbMaxDiff(g, expected, inner), rgbMaxDiffFromClearGrey(g, outer),
                            wgl->compositeBakedMode()};
        };
        // Route 1: ordinary paused refresh; displayFrame pulls the special
        // frame from MainWindow's provider and presents it (presentDisplay).
        ordinaryRefresh();
        const QImage frameGrab = wgrab();
        routeFrameInner = rgbMaxDiff(frameGrab, expected, inner);
        routeFrameOuter = rgbMaxDiffFromClearGrey(frameGrab, outer);
        routeFrameBaked = wgl->compositeBakedMode();
        // Route 2: refreshSpecialClipPreview.
        ordinaryRefresh();
        const PreState preRefresh = sentinelPre();
        window.refreshSpecialClipPreview();
        const QImage refreshGrab = wgrab();
        routeRefreshInner = rgbMaxDiff(refreshGrab, expected, inner);
        routeRefreshOuter = rgbMaxDiffFromClearGrey(refreshGrab, outer);
        routeRefreshBaked = wgl->compositeBakedMode();
        // Route 3: applyStillCompareConfig.
        ordinaryRefresh();
        const PreState preStill = sentinelPre();
        window.applyStillCompareConfig();
        const QImage stillGrab = wgrab();
        routeStillInner = rgbMaxDiff(stillGrab, expected, inner);
        routeStillOuter = rgbMaxDiffFromClearGrey(stillGrab, outer);
        routeStillBaked = wgl->compositeBakedMode();
        std::cerr << "special-composite G5 route=pausedDisplayFrame innerMaxDiff=" << routeFrameInner
                  << " outerClearDiff=" << routeFrameOuter << " baked=" << routeFrameBaked << '\n'
                  << "special-composite G5 route=refreshSpecialClipPreview innerMaxDiff=" << routeRefreshInner
                  << " outerClearDiff=" << routeRefreshOuter << " baked=" << routeRefreshBaked
                  << " preInner=" << preRefresh.inner << " preOuter=" << preRefresh.outer
                  << " preBaked=" << preRefresh.baked << '\n'
                  << "special-composite G5 route=applyStillCompareConfig innerMaxDiff=" << routeStillInner
                  << " outerClearDiff=" << routeStillOuter << " baked=" << routeStillBaked
                  << " preInner=" << preStill.inner << " preOuter=" << preStill.outer
                  << " preBaked=" << preStill.baked << '\n';
        sentinelsOk = preRefresh.inner > 2 && preRefresh.outer > 2 && !preRefresh.baked
                      && preStill.inner > 2 && preStill.outer > 2 && !preStill.baked;

        // Display-layer parity on the same pausedDisplayFrame route: the special
        // frame must keep the title bake and the exposure aid that the
        // ordinary frame carries (ACES / safe zone share the same helper).
        {
            setGradedClip(true);
            ordinaryRefresh();
            const QImage aidOffGrab = wgrab();
            ClipInfo textClip = gradedClip;
            EnhancedTextOverlay title;
            title.text = QStringLiteral("TITLE");
            title.font = QFont(QStringLiteral("Arial"), 40, QFont::Bold);
            title.color = Qt::white;
            title.x = 0.5;
            title.y = 0.5;
            textClip.textManager.addOverlay(title);
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{textClip}},
                QVector<QVector<ClipInfo>>{}, t5, -1.0, -1.0, 100);
            timeline->setRotoEntries(QHash<QString, RotoClipEntry>{{rotoKey, rotoEntry}});
            const QImage textExpected = tlrender::renderFrameAt(timeline, qint64(t5 * 1000000.0), size);
            wp->setHiddenTextOverlayIndex(0);
            wp->setTextOverlays(timeline->timelineTextOverlays());
            const QImage textHiddenGrab = wgrab();
            wp->setHiddenTextOverlayIndex(-1);
            const QImage textGrab = wgrab();
            // Every pixel of `inner`, glyph edges included: the grab is read
            // back 1:1 from the Nearest-sampled frame texture.
            textBakeInner = rgbMaxDiff(textGrab, textExpected, inner);
            // Control: the same frame sampled Linear (the user default) is
            // resampled at the device size and misses on the glyph edges, so
            // the every-pixel check above depends on the Nearest switch.
            wgl->setFrameTextureNearestForTest(false);
            textLinearInner = rgbMaxDiff(wgrab(), textExpected, inner);
            wgl->setFrameTextureNearestForTest(true);
            // At an odd integer DPR (1, 3, ...) every sampled device-pixel
            // centre lands on a texel centre, so Linear == Nearest and the
            // control above cannot separate them; it is only required otherwise.
            const qreal d = wglGrab.dpr;
            texelCentred = std::abs(d - std::round(d)) < 1e-6 && (int(std::round(d)) % 2) == 1;
            int whiteStrokePx = 0;
            for (int y = inner.top(); y <= inner.bottom(); ++y)
                for (int x = inner.left(); x <= inner.right(); ++x)
                    if (std::abs(qRed(textExpected.pixel(x, y)) - qRed(expected.pixel(x, y))) > 60)
                        ++whiteStrokePx;
            textStrokePx = whiteStrokePx;
            textBakeVisible = rgbMaxDiff(textGrab, textHiddenGrab, inner);
            textExpectedVisible = rgbMaxDiff(textExpected, expected, inner);
            wp->setTextOverlays({});
            setGradedClip(true);
            wp->setExposureAidMode(exposureaid::AidMode::FalseColor);
            ordinaryRefresh();
            const QImage aidOnGrab = wgrab();
            aidVisible = rgbMaxDiff(aidOnGrab, aidOffGrab, QRect(0, 0, kW, kH));
            aidOuterClear = rgbMaxDiffFromClearGrey(aidOnGrab, outer);
            aidUploadFormat = wgl->lastUploadFormatForTest();
            wp->setExposureAidMode(exposureaid::AidMode::None);
            ordinaryRefresh();
            aidRestored = rgbMaxDiff(wgrab(), aidOffGrab, QRect(0, 0, kW, kH));
            std::cerr << "special-composite G5 text innerMaxDiff=" << textBakeInner
                      << " linearControlInnerMaxDiff=" << textLinearInner
                      << " texelCentred=" << texelCentred
                      << " vsHiddenMaxDiff=" << textBakeVisible
                      << " strokePx=" << textStrokePx << " expectedTextVisible=" << textExpectedVisible
                      << " aidVisibleMaxDiff=" << aidVisible
                      << " aidOuterClearDiff=" << aidOuterClear
                      << " aidUploadFormat=" << int(aidUploadFormat)
                      << " aidRestoredMaxDiff=" << aidRestored << '\n';
        }

        // Ordinary frameComposited payload of the t5 display at the end of the
        // seek/stop block, used by the direct-redisplay check after it.
        QImage t5Ordinary;

        // Player-driven seek / stop with a non-identity videoScale: the special
        // frame must be the one built at the DISPLAYED time (the Timeline
        // playhead is only updated after the display) and must stay baked, so
        // the entry's scale/offset is not applied a second time.
        {
            ClipInfo scaledClip = gradedClip;
            scaledClip.videoScale = 0.8;
            const double tStale = 0.2;
            const double tSeek = 0.8;
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{scaledClip}},
                QVector<QVector<ClipInfo>>{}, tStale, -1.0, -1.0, 100);
            timeline->setRotoEntries(QHash<QString, RotoClipEntry>{{rotoKey, rotoEntry}});
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            const QImage seekExpected = tlrender::renderFrameAt(timeline, qint64(tSeek * 1000000.0), size);
            const QImage staleExpected = tlrender::renderFrameAt(timeline, qint64(tStale * 1000000.0), size);
            const QImage zeroExpected = tlrender::renderFrameAt(timeline, 0, size);
            // The roto rectangle shrinks with videoScale about the frame
            // centre; judge its interior (2 px inside the scaled edges).
            const QPointF centre(kW / 2.0, kH / 2.0);
            const QRect seekRect = QRectF(centre + (QPointF(inner.topLeft()) - centre) * scaledClip.videoScale,
                                          centre + (QPointF(inner.left() + inner.width(),
                                                            inner.top() + inner.height()) - centre)
                                                       * scaledClip.videoScale)
                                       .toAlignedRect().adjusted(2, 2, -2, -2);
            seekStaleDiff = rgbMaxDiff(staleExpected, seekExpected, seekRect);
            seekZeroDiff = rgbMaxDiff(zeroExpected, seekExpected, seekRect);
            {
                bool seeked = false;
                const auto conn = QObject::connect(wp, &VideoPlayer::positionChanged, wp,
                    [&](double pos) { if (std::abs(pos - tSeek) < 0.001) seeked = true; });
                wp->seek(int(tSeek * 1000));
                (void)QTest::qWaitFor([&] { return seeked; }, 15000);
                QObject::disconnect(conn);
                QApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            const QImage seekGrab = wgrab();
            seekInner = rgbMaxDiff(seekGrab, seekExpected, seekRect);
            seekOuter = rgbMaxDiffFromClearGrey(seekGrab, outer);
            seekBaked = wgl->compositeBakedMode();
            wp->stop();
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            const QImage stopGrab = wgrab();
            stopInner = rgbMaxDiff(stopGrab, zeroExpected, seekRect);
            stopOuter = rgbMaxDiffFromClearGrey(stopGrab, outer);
            stopBaked = wgl->compositeBakedMode();
            std::cerr << "special-composite G5 seek seqActive=" << wp->hasActiveSequence()
                      << " staleVsSeek=" << seekStaleDiff << " zeroVsSeek=" << seekZeroDiff
                      << " seekInner=" << seekInner << " seekOuter=" << seekOuter
                      << " seekRect=" << seekRect.x() << ',' << seekRect.y() << ','
                      << seekRect.width() << 'x' << seekRect.height()
                      << " seekBaked=" << seekBaked << " stopInner=" << stopInner
                      << " stopOuter=" << stopOuter << " stopBaked=" << stopBaked << '\n';
            seekOk = seekStaleDiff > 2 && seekZeroDiff > 2 && seekInner <= 2 && seekOuter <= 2
                     && seekBaked && stopInner <= 2 && stopOuter <= 2 && stopBaked;
            // Back to the unscaled clip at t5 so the restoration below is
            // compared with plainGrab at the same frame.
            setGradedClip(true);
            bool seeked = false;
            const auto conn = QObject::connect(wp, &VideoPlayer::positionChanged, wp,
                [&](double pos) { if (std::abs(pos - t5) < 0.001) seeked = true; });
            const auto payloadConn = QObject::connect(wp, &VideoPlayer::frameComposited, wp,
                [&](const QImage &frame) { t5Ordinary = frame; });
            wp->seek(int(t5 * 1000));
            (void)QTest::qWaitFor([&] { return seeked && !t5Ordinary.isNull(); }, 15000);
            QObject::disconnect(conn);
            QApplication::processEvents(QEventLoop::AllEvents, 100);
            QObject::disconnect(payloadConn);
        }

        // Direct MainWindow redisplay (effect-library preview off / node
        // graph): it skips presentDisplay and calls only
        // applyStillCompareForDisplay, which must end the special latch so the
        // ordinary frame is shown with GL grade/effects on again.
        const bool directPreBaked = wgl->compositeBakedMode();
        wgl->displayFrame(wp->applyStillCompareForDisplay(t5Ordinary, wp->timelinePositionUs(), true));
        const QImage directGrab = wgrab();
        const bool directBaked = wgl->compositeBakedMode();
        const bool directFx = wgl->effectsEnabled();
        const int directDiff = rgbMaxDiff(directGrab, plainGrab, QRect(0, 0, kW, kH));
        std::cerr << "special-composite G5 direct ordinaryNull=" << t5Ordinary.isNull()
                  << " preBaked=" << directPreBaked << " directBaked=" << directBaked
                  << " directFx=" << directFx << " directVsPlainMaxDiff=" << directDiff << '\n';
        // Re-present the special so the restoration below starts from it.
        ordinaryRefresh();

        // Restoration: dropping the roto returns the ordinary frame to baked
        // off and GL grade on, the same as the plain (no roto) state above.
        timeline->setRotoEntries(QHash<QString, RotoClipEntry>{});
        ordinaryRefresh();
        const QImage restoredGrab = wgrab();
        restoreBakedOk = !wgl->compositeBakedMode() && !baseBaked;
        restoreFxOk = wgl->effectsEnabled() && baseFx;
        const int restoredDiff = rgbMaxDiff(restoredGrab, plainGrab, QRect(0, 0, kW, kH));
        const int restoredVsSpecialOuter = rgbMaxDiffFromClearGrey(restoredGrab, outer);
        restoreImageOk = restoredDiff <= 2 && restoredVsSpecialOuter > 2;
        std::cerr << "special-composite G5 restore baseBaked=" << baseBaked << " baseFx=" << baseFx
                  << " restoredBaked=" << wgl->compositeBakedMode()
                  << " restoredFx=" << wgl->effectsEnabled()
                  << " restoredVsPlainMaxDiff=" << restoredDiff
                  << " restoredOuterClearDiff=" << restoredVsSpecialOuter << '\n';
        std::cerr << "special-composite G5 windowGrab dpr=" << wglGrab.dpr
                  << " widgetLogical=" << wgl->width() << 'x' << wgl->height()
                  << " rawGrab=" << wglGrab.rawSize.width() << 'x' << wglGrab.rawSize.height()
                  << " rawSizeOk=" << wglGrab.rawSizeOk << " grabs=" << wglGrab.grabs << '\n';
        routesOk = wOk && wglGrab.rawSizeOk && !plainGrab.isNull() && routeFrameInner <= 2 && routeRefreshInner <= 2
                   && routeStillInner <= 2 && routeFrameOuter <= 2 && routeRefreshOuter <= 2
                   && routeStillOuter <= 2 && routeFrameBaked && routeRefreshBaked && routeStillBaked
                   && sentinelsOk
                   && textBakeInner <= 2 && (texelCentred || textLinearInner > 2) && textStrokePx >= 50 && textBakeVisible > 2 && textExpectedVisible > 2
                   && aidVisible > 2 && aidOuterClear <= 2 && aidUploadFormat == QImage::Format_ARGB32_Premultiplied
                   && seekOk && aidRestored <= 2
                   && !t5Ordinary.isNull() && !directBaked && directFx && directDiff <= 2
                   && restoreBakedOk && restoreFxOk && restoreImageOk;
    }

    // G5: the single entry bakes the frame (no second grade).
    player.displaySpecialComposite(special);
    const QImage bakedGrab = grab();
    const int g5Inner = rgbMaxDiff(bakedGrab, expected, inner);
    const int g5Outer = rgbMaxDiffFromClearGrey(bakedGrab, outer);
    std::cerr << "special-composite G5 dpr=" << glGrab.dpr
              << " widgetLogical=" << gl->width() << 'x' << gl->height()
              << " rawGrab=" << glGrab.rawSize.width() << 'x' << glGrab.rawSize.height()
              << " rawSizeOk=" << glGrab.rawSizeOk
              << " specialFormat=" << int(special.format())
              << " innerMaxDiff=" << g5Inner << " outerClearDiff=" << g5Outer
              << " gradeVisibleMaxDiff=" << gradeVisible << '\n';
    tally.gate(5, glGrab.rawSizeOk && !bakedGrab.isNull() && !special.isNull() && gradeVisible > 8
                   && g5Inner <= 2 && g5Outer <= 2 && routesOk,
               "displaySpecialComposite output differs from renderFrameAt (see values above)");
    std::cerr << "special-composite G6 innerMaxDiff=" << g6Inner
              << " (HEAD path: displayFrame without baked mode)\n";
    tally.gate(6, glGrab.rawSizeOk && !headGrab.isNull() && g6Inner > 2,
               "plain displayFrame no longer re-grades; the negative control is vacuous");

    // G7: a half-transparent ARGB32_Premultiplied special composite reaches the
    // screen as a correct alpha composite over the background. GL's output is
    // composited opaque, so alpha is judged through the colour: straight
    // (200,100,40) at alpha 128 over the 0.1 grey clear colour is about
    // (113,63,33). Negative control: forcing the RGBA8888 (straight) upload the
    // US-1106 draft specified makes the same frame about (211,112,52).
    QImage half(kW, kH, QImage::Format_ARGB32);
    half.fill(QColor(200, 100, 40, 128));
    const QImage halfPremul = half.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    player.displaySpecialComposite(halfPremul);
    const QImage::Format translucentFormat = gl->lastUploadFormatForTest();
    const QRgb routed = grab().pixel(kW / 2, kH / 2);
    gl->displayFrame(halfPremul.convertToFormat(QImage::Format_RGBA8888));
    const QRgb straight = grab().pixel(kW / 2, kH / 2);
    const int expectR = 113, expectG = 63, expectB = 33;
    const int routedErr = std::max({std::abs(qRed(routed) - expectR), std::abs(qGreen(routed) - expectG),
                                    std::abs(qBlue(routed) - expectB)});
    const int straightErr = std::max({std::abs(qRed(straight) - expectR), std::abs(qGreen(straight) - expectG),
                                      std::abs(qBlue(straight) - expectB)});
    std::cerr << "special-composite G7 dpr=" << glGrab.dpr
              << " rawGrab=" << glGrab.rawSize.width() << 'x' << glGrab.rawSize.height()
              << " rawSizeOk=" << glGrab.rawSizeOk
              << " uploadFormat=" << int(translucentFormat)
              << " routedRgb=" << qRed(routed) << ',' << qGreen(routed) << ',' << qBlue(routed)
              << " routedErr=" << routedErr
              << " straightControlRgb=" << qRed(straight) << ',' << qGreen(straight) << ','
              << qBlue(straight) << " straightControlErr=" << straightErr << '\n';
    tally.gate(7, glGrab.rawSizeOk && translucentFormat == QImage::Format_ARGB32_Premultiplied
                   && routedErr <= 2 && straightErr > 2,
               "translucent special composite no longer composites correctly (see values above)");

    return tally.finish("special-composite");
}
