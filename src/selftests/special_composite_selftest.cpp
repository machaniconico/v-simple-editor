// US-708: MainWindow::buildSpecialClipComposite (special-data preview path) must
// run the same native FX stack (mesh warp / puppet pins / FX pack) as the export
// path tlrender::renderFrameAt, so preview and export agree.
#include "../GLPreview.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../MainWindow.h"
#include "../RotoClipEntry.h"
#include "../Timeline.h"
#include "../SequenceExport.h"
#include "../TrackMatteKey.h"
#include "../VideoPlayer.h"
#include "../TimelineFrameRenderer.h"
#include "../WarpDistortion.h"
#include "../WiggleTransform.h"
#include "../AudioKeyframes.h"
#include "../TrimOps.h"
#include "../clipanim/ClipAnim.h"
#include "../libavcore/AudioExtract.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QPainter>
#include <QImage>
#include <QOpenGLContext>
#include <QRect>
#include <QSignalSpy>
#include <QTest>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <utility>
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
        for (int g = 1; g <= 10; ++g)
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
        for (int g = 5; g <= 10; ++g)
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
        tally.skip(9, "no usable QOpenGLWidget context");
        tally.skip(10, "no usable QOpenGLWidget context");
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

    // G8: a file-backed RenderQueue job (resolveTimeline) must put the saved
    // time-remap entries and the freeze-frame curve (which lives only on the
    // ClipInfo until populateProjectData turns it into an entry) back on the
    // clips. The job is a one-frame PNG sequence export (lossless, still goes
    // through resolveTimeline and tlrender::renderFrameAt), compared with the
    // live Timeline frame over the WHOLE image: same size and every pixel
    // equal. The fixture's frame i is a uniform colour with R = 25 * i, so the
    // centre pixel's red (diagnostic only) names the source frame. The 0.5x
    // remap shows frame 4 at timeline 0.8 s (speed alone would show frame 8);
    // the freeze holds the in-point frame 4 (speed alone would show frame 7 at
    // 0.3 s). Negative controls: the same projects exported WITHOUT their
    // entries are the HEAD behaviour and must differ from the live frame, with
    // the centre red more than 50 apart. A failed save, export or decode is
    // never a passing control: every export reports success and a decoded image.
    struct G8Export {
        bool ok = false;
        QImage image;
    };
    struct G8Diff {
        bool sameSize = false;
        qint64 differing = -1;
        int maxChannel = -1;
        bool equal() const { return sameSize && differing == 0; }
    };
    const auto centreRed = [](const QImage &image) {
        return image.isNull() ? -1000 : image.convertToFormat(QImage::Format_RGB888).pixelColor(kW / 2, kH / 2).red();
    };
    // The sequence writer flattens onto black into RGB888 (keepAlpha off); do
    // the same to the live frame so both sides are in one format.
    const auto flatten = [](const QImage &image) {
        QImage opaque(image.size(), QImage::Format_RGB888);
        opaque.fill(Qt::black);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
        painter.end();
        return opaque;
    };
    const auto diffImages = [&](const QImage &a, const QImage &b) {
        G8Diff diff;
        if (a.isNull() || b.isNull()) return diff;
        diff.sameSize = a.size() == b.size();
        if (!diff.sameSize) return diff;
        const QImage left = a.convertToFormat(QImage::Format_RGB888);
        const QImage right = b.convertToFormat(QImage::Format_RGB888);
        diff.differing = 0;
        diff.maxChannel = 0;
        for (int y = 0; y < left.height(); ++y)
            for (int x = 0; x < left.width(); ++x) {
                const QColor l = left.pixelColor(x, y);
                const QColor r = right.pixelColor(x, y);
                const int d = std::max({std::abs(l.red() - r.red()), std::abs(l.green() - r.green()),
                                        std::abs(l.blue() - r.blue())});
                if (d > 0) ++diff.differing;
                diff.maxChannel = std::max(diff.maxChannel, d);
            }
        return diff;
    };
    struct SequenceTestGuard {
        const bool previous = sequenceexport::enabledForTest();
        SequenceTestGuard() { sequenceexport::setEnabledForTest(true); }
        ~SequenceTestGuard() { sequenceexport::setEnabledForTest(previous); }
    };
    const auto exportFrame = [&](ProjectData project, const QString &name, double targetSec) {
        G8Export result;
        const QString projectPath = dir.filePath(name + QStringLiteral(".veditor"));
        const QString folder = dir.filePath(name);
        if (!ProjectFile::save(projectPath, project))
            return result;
        const qint64 startUs = qint64(targetSec * 1000000.0);
        RenderJob renderJob;
        renderJob.projectFilePath = projectPath;
        renderJob.outputPath = folder;
        renderJob.width = kW;
        renderJob.height = kH;
        renderJob.startUs = startUs;
        renderJob.endUs = startUs + qint64(1000000.0 / kFps);
        renderJob.exportConfig.insert(QStringLiteral("fps"), kFps);
        renderJob.exportConfig.insert(QStringLiteral("imageSequence"), true);
        renderJob.exportConfig.insert(QStringLiteral("sequenceFolder"), folder);
        renderJob.exportConfig.insert(QStringLiteral("sequencePattern"), QStringLiteral("g8_%04d.png"));
        renderJob.exportConfig.insert(QStringLiteral("sequenceDigits"), 4);
        renderJob.exportConfig.insert(QStringLiteral("sequenceStart"), 0);
        bool completed = false;
        {
            SequenceTestGuard guard;
            RenderQueue renderQueue;
            QSignalSpy done(&renderQueue, &RenderQueue::jobCompletedUuid);
            renderQueue.addJob(renderJob);
            renderQueue.start();
            if (done.isEmpty()) done.wait(120000);
            completed = !done.isEmpty() && done.first().value(1).toBool();
        }
        result.image = QImage(QDir(folder).filePath(QStringLiteral("g8_0000.png")));
        result.ok = completed && !result.image.isNull() && QDir(folder).entryList(QDir::Files).size() == 1;
        return result;
    };
    const double g8StepUs = 1000000.0 / kFps;
    const auto liveFrame = [&](double targetSec) {
        return flatten(tlrender::renderFrameAt(timeline, qint64(targetSec * 1000000.0), size, g8StepUs));
    };

    // (1)(2) remap entry -> file-backed export == live Timeline.
    ClipInfo halfSpeed = base;
    halfSpeed.filePath = varyPath;
    halfSpeed.timeRemapCurve.sourceFps = kFps;
    halfSpeed.timeRemapCurve.addKey(0.0, 0.0);
    halfSpeed.timeRemapCurve.addKey(1.0, 0.5);
    setClip(halfSpeed);
    const double g8TimelineSec = 0.8;
    const double g8RemapLocalSec = g8TimelineSec - halfSpeed.leadInSec;
    const double g8SourceSec = halfSpeed.inPoint + halfSpeed.timeRemapCurve.srcTimeAt(g8RemapLocalSec);
    const QImage liveRemap = liveFrame(g8TimelineSec);
    ClipInfo halfSpeedStored = halfSpeed;
    halfSpeedStored.timeRemapCurve = timeremap::TimeRemapCurve();
    ProjectData remapProject;
    remapProject.videoTracks = QVector<QVector<ClipInfo>>{{halfSpeedStored}};
    TimeRemapClipEntry remapEntry;
    remapEntry.clipId = QStringLiteral("0:0");
    remapEntry.curve = halfSpeed.timeRemapCurve;
    remapProject.timeRemapClipEntries = {remapEntry};
    const G8Export remapExport = exportFrame(remapProject, QStringLiteral("g8-remap"), g8TimelineSec);
    ProjectData remapNoEntry = remapProject;
    remapNoEntry.timeRemapClipEntries.clear();
    const G8Export remapHead = exportFrame(remapNoEntry, QStringLiteral("g8-remap-head"), g8TimelineSec);
    const G8Diff remapDiff = diffImages(remapExport.image, liveRemap);
    const G8Diff remapHeadDiff = diffImages(remapHead.image, liveRemap);
    const int liveRemapRed = centreRed(liveRemap);
    const int remapExportRed = centreRed(remapExport.image);
    const int remapHeadRed = centreRed(remapHead.image);
    const bool remapOk = !liveRemap.isNull() && remapExport.ok && remapHead.ok
        && remapDiff.equal() && !remapHeadDiff.equal()
        && std::abs(remapHeadRed - liveRemapRed) > 50;

    // (3) freeze frame saved through populateProjectData.
    ClipInfo frozen = base;
    frozen.filePath = varyPath;
    frozen.inPoint = 0.4;
    frozen.outPoint = 1.0;
    frozen.timeRemapCurve.blendMode = timeremap::FrameBlendMode::NearestFrame;
    frozen.timeRemapCurve.sourceFps = kFps;
    frozen.timeRemapCurve.addKey(0.0, 0.0);
    setClip(frozen);
    window.m_timeRemapClipEntries.clear();
    const double g8FreezeTimelineSec = 0.3;
    const double g8FreezeLocalSec = g8FreezeTimelineSec - frozen.leadInSec;
    const double g8FreezeSourceSec = frozen.inPoint + frozen.timeRemapCurve.srcTimeAt(g8FreezeLocalSec);
    const QImage liveFreeze = liveFrame(g8FreezeTimelineSec);
    ProjectData freezeProject;
    window.populateProjectData(freezeProject);
    const bool freezeEntryPresent = freezeProject.timeRemapClipEntries.size() == 1;
    const G8Export freezeExport = exportFrame(freezeProject, QStringLiteral("g8-freeze"), g8FreezeTimelineSec);
    ProjectData freezeNoEntry = freezeProject;
    freezeNoEntry.timeRemapClipEntries.clear();
    const G8Export freezeHead = exportFrame(freezeNoEntry, QStringLiteral("g8-freeze-head"), g8FreezeTimelineSec);
    const G8Diff freezeDiff = diffImages(freezeExport.image, liveFreeze);
    const G8Diff freezeHeadDiff = diffImages(freezeHead.image, liveFreeze);
    const int liveFreezeRed = centreRed(liveFreeze);
    const int freezeExportRed = centreRed(freezeExport.image);
    const int freezeHeadRed = centreRed(freezeHead.image);
    const bool freezeOk = freezeEntryPresent && !liveFreeze.isNull() && freezeExport.ok && freezeHead.ok
        && freezeDiff.equal() && !freezeHeadDiff.equal()
        && std::abs(freezeHeadRed - liveFreezeRed) > 50;

    // (4) no entries: file-backed export == live Timeline (regression).
    ClipInfo plainClip = base;
    plainClip.filePath = varyPath;
    setClip(plainClip);
    const double g8PlainLocalSec = g8TimelineSec - plainClip.leadInSec;
    const double g8PlainSourceSec = plainClip.inPoint + g8PlainLocalSec * plainClip.speed;
    const QImage livePlain = liveFrame(g8TimelineSec);
    ProjectData plainProject;
    plainProject.videoTracks = QVector<QVector<ClipInfo>>{{plainClip}};
    const G8Export plainExport = exportFrame(plainProject, QStringLiteral("g8-plain"), g8TimelineSec);
    const G8Diff plainDiff = diffImages(plainExport.image, livePlain);
    const int livePlainRed = centreRed(livePlain);
    const int plainExportRed = centreRed(plainExport.image);
    const bool plainOk = !livePlain.isNull() && plainExport.ok && plainDiff.equal();

    const auto diffText = [](const G8Diff &d) {
        return std::string("sameSize=") + (d.sameSize ? "1" : "0") + " differingPixels=" + std::to_string(d.differing)
            + " maxChannelDiff=" + std::to_string(d.maxChannel);
    };
    std::cerr << "special-composite G8 (1)(2) remap timelineSec=" << g8TimelineSec
              << " localSec=" << g8RemapLocalSec << " sourceSec=" << g8SourceSec
              << " liveRed=" << liveRemapRed << " exportRed=" << remapExportRed
              << " noEntryControlRed=" << remapHeadRed
              << " exportOk=" << remapExport.ok << " controlOk=" << remapHead.ok
              << " export[" << diffText(remapDiff) << "] noEntryControl[" << diffText(remapHeadDiff)
              << "] ok=" << remapOk << '\n';
    std::cerr << "special-composite G8 (3) freeze timelineSec=" << g8FreezeTimelineSec
              << " localSec=" << g8FreezeLocalSec << " sourceSec=" << g8FreezeSourceSec
              << " entryFromPopulate=" << freezeEntryPresent
              << " liveRed=" << liveFreezeRed << " exportRed=" << freezeExportRed
              << " noEntryControlRed=" << freezeHeadRed
              << " exportOk=" << freezeExport.ok << " controlOk=" << freezeHead.ok
              << " export[" << diffText(freezeDiff) << "] noEntryControl[" << diffText(freezeHeadDiff)
              << "] ok=" << freezeOk << '\n';
    std::cerr << "special-composite G8 (4) plain timelineSec=" << g8TimelineSec
              << " localSec=" << g8PlainLocalSec << " sourceSec=" << g8PlainSourceSec
              << " liveRed=" << livePlainRed << " exportRed=" << plainExportRed
              << " exportOk=" << plainExport.ok
              << " export[" << diffText(plainDiff) << "] ok=" << plainOk << '\n';
    tally.gate(8, remapOk && freezeOk && plainOk,
               "file-backed export lost the saved time-remap or freeze-frame (see values above)");

    // G9 (US-1504): the wiggle is ClipInfo::wiggle, evaluated by the one shared overlay
    // (clipanim::applyDynamicOverlay) on top of keyframes -> autoOrient, so export
    // (renderFrameAt), both previews and the special composite carry it once.
    //   (1) renderFrameAt at t = 0.5 with the wiggle (seed 3, 20 px) differs from without
    //       (HEAD: the export ignores the wiggle sidecar -> equal).
    //   (2') effectiveTransformAt(clip, 0.5, env{1920x1080}).videoDx == base + off.x / 1920
    //       (1e-9, also dy), the two-argument form == base, two env calls byte-equal.
    //   (3) a keyframe-less wiggle clip: the GLPreview motion (makePreviewMotion through
    //       paintGL -> videoSourceDx/Dy), the VideoPlayer layer values (cpu-composite record)
    //       and renderFrameAt (== a static clip holding the evaluated transform, pixel-exact)
    //       all carry the env value (mutation control: the three gates left on
    //       keyframes.hasAnyKeyframes() -> the base values).
    //   (4') time remap + wiggle, single clip: the window's GL upload of the special composite
    //       == renderFrameAt (inner rect), and the baked frame's GL motion is keyframes-only
    //       (no second wiggle; mutation control: the env form under compositeBaked).
    //   (5) arrange (Left) writes the same videoDx with and without the wiggle.
    //   (6) a 1920x1080 project rendered at 1280x720 and 1920x1080 both equal a static clip
    //       holding the wiggle normalized by the 1920x1080 project canvas (control: by 1280).
    timeline->setProjectOutputConfig(kW, kH, true, kFps); // MainWindow keeps it with m_projectConfig
    wiggle::WiggleParams g9Wiggle;
    g9Wiggle.enabled = true;
    g9Wiggle.seed = 3;
    g9Wiggle.positionAmplitude = QPointF(20.0, 20.0);
    const double g9t = 0.5;
    const wiggle::WiggleOffset g9Off = wiggle::evaluate(g9Wiggle, g9t);
    // Sanity band on the shared evaluator itself: a real, bounded pixel offset.
    const bool g9OffSane = std::abs(g9Off.positionOffset.x()) > 0.5 && std::abs(g9Off.positionOffset.x()) <= 40.0
        && std::abs(g9Off.positionOffset.y()) <= 40.0;
    const auto staticHolding = [](ClipInfo clip, const clipgeom::ClipTransform &tr) {
        clip.wiggle = wiggle::WiggleParams{};
        clip.videoScale = tr.videoScale;
        clip.videoDx = tr.videoDx;
        clip.videoDy = tr.videoDy;
        clip.rotation2DDegrees = tr.rotationDeg;
        return clip;
    };
    const auto renderOne = [](const ClipInfo &clip, QSize project, double sec, QSize out) {
        Timeline tl;
        tl.setProjectOutputConfig(project.width(), project.height(), true, kFps);
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
        return tlrender::renderFrameAt(&tl, qint64(sec * 1000000.0), out);
    };

    // (1)
    ClipInfo g9Clip = base;
    g9Clip.wiggle = g9Wiggle;
    const QImage g9Plain = renderOne(base, size, g9t, size);
    const QImage g9Wig = renderOne(g9Clip, size, g9t, size);
    const DiffStat g9d1 = compare(g9Wig, g9Plain);
    const bool g9ok1 = g9OffSane && g9d1.sizeMatch && g9d1.diffPixels > 50;

    // (2')
    ClipInfo g9Based = g9Clip;
    g9Based.videoDx = 0.1;
    g9Based.videoDy = -0.05;
    clipanim::EvalEnv env1080;
    env1080.canvas = QSize(1920, 1080);
    const clipgeom::ClipTransform g9EnvA = clipanim::effectiveTransformAt(g9Based, g9t, env1080);
    const clipgeom::ClipTransform g9EnvB = clipanim::effectiveTransformAt(g9Based, g9t, env1080);
    const clipgeom::ClipTransform g9Two = clipanim::effectiveTransformAt(g9Based, g9t);
    const double g9ExpDx = 0.1 + g9Off.positionOffset.x() / 1920.0;
    const double g9ExpDy = -0.05 + g9Off.positionOffset.y() / 1080.0;
    const bool g9ok2 = g9OffSane && std::abs(g9EnvA.videoDx - g9ExpDx) <= 1e-9
        && std::abs(g9EnvA.videoDy - g9ExpDy) <= 1e-9 && g9Two.videoDx == 0.1 && g9Two.videoDy == -0.05
        && std::memcmp(&g9EnvA, &g9EnvB, sizeof g9EnvA) == 0;

    // (3)
    Timeline g9tl;
    g9tl.setProjectOutputConfig(kW, kH, true, kFps);
    g9tl.restoreFromProject(QVector<QVector<ClipInfo>>{{g9Clip}}, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    g9tl.refreshPlaybackSequence();
    const clipanim::EvalEnv g9Env = tlrender::dynamicEvalEnv(&g9tl);
    const auto envAt = [&](double localSec) { return clipanim::effectiveTransformAt(g9Clip, localSec, g9Env); };
    const clipgeom::ClipTransform g9Exp05 = envAt(g9t);
    const QImage g9Static = renderOne(staticHolding(base, g9Exp05), size, g9t, size);
    const DiffStat g9d3 = compare(g9Wig, g9Static);
    double glDx = 0.0, glDy = 0.0, glLocal = -1.0, vpDx = 0.0, vpDy = 0.0, vpLocal = -1.0;
    clipgeom::ClipTransform glExp, vpExp;
    bool glBaked = true, vpRecorded = false, g9Seeked = false;
    int vpAttempts = 0;
    {
        VideoPlayer vp;
        vp.resize(kW, kH);
        vp.setCanvasSize(kW, kH);
        vp.setProjectOutputSize(size);
        vp.glPreview()->setTimeline(&g9tl);
        vp.setSequence(g9tl.computePlaybackSequence());
        vp.show();
        QSignalSpy vpFrames(&vp, &VideoPlayer::frameComposited);
        vp.seek(int(g9t * 1000));
        g9Seeked = QTest::qWaitFor([&] {
            return vpFrames.count() > 0 && std::llabs(vp.timelinePositionUs() - qint64(g9t * 1000000.0)) < 50000;
        }, 15000);
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        (void)vp.glPreview()->grabFramebuffer(); // paintGL resolves the clip motion
        glBaked = vp.glPreview()->compositeBakedMode();
        glDx = vp.glPreview()->videoSourceDx();
        glDy = vp.glPreview()->videoSourceDy();
        glLocal = double(vp.timelinePositionUs()) / 1000000.0;
        glExp = envAt(glLocal);
        // The cpu-composite path (a CPU-only preview stack) records the layer values
        // applyLayerMotionOpacity produced; a paused seek alone may leave its V1 base
        // empty, so a short playback fills it (as in video-player-parent).
        const auto cpuRecord = [&]() {
            vp.setParentAdmissionObservationForTest(true);
            const int before = vpFrames.count();
            vp.setPreviewEffects({VideoEffect::createFilmGrain(0.3, 1, 0.0, false)});
            (void)QTest::qWaitFor([&] { return vpFrames.count() > before; }, 3000);
            const auto records = vp.parentAdmissionRecordsForTest();
            vp.setPreviewEffects({});
            vp.setParentAdmissionObservationForTest(false);
            ++vpAttempts;
            for (const auto &record : records) {
                if (record.path != QLatin1String("cpu-composite") || record.sourceTrack != 0) continue;
                vpRecorded = true;
                vpDx = record.localTransform.videoDx;
                vpDy = record.localTransform.videoDy;
                vpLocal = double(record.timelineUs) / 1000000.0;
            }
        };
        cpuRecord();
        if (!vpRecorded) {
            const int beforePlay = vpFrames.count();
            vp.play();
            (void)QTest::qWaitFor([&] { return vpFrames.count() >= beforePlay + 3; }, 5000);
            vp.pause();
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            cpuRecord();
        }
        vpExp = envAt(vpLocal);
        vp.hide();
    }
    const bool glMoves = std::abs(glExp.videoDx - base.videoDx) > 1e-4;
    const bool g9ok3 = g9Seeked && !glBaked && glMoves
        && g9Env.canvas == size
        && std::abs(glDx - glExp.videoDx) <= 1e-9 && std::abs(glDy - glExp.videoDy) <= 1e-9
        && vpRecorded && std::abs(vpDx - vpExp.videoDx) <= 1e-9 && std::abs(vpDy - vpExp.videoDy) <= 1e-9
        && std::abs(vpExp.videoDx - base.videoDx) > 1e-4
        && g9d3.sizeMatch && g9d3.maxDiff == 0 && g9d1.diffPixels > 50;

    // (4') the window route: GL shows the special composite of a time-remap + wiggle clip.
    ClipInfo remapWig = remapped;
    remapWig.wiggle = g9Wiggle;
    timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{remapped}},
        QVector<QVector<ClipInfo>>{}, g9t, -1.0, -1.0, 100);
    const QImage remapNoWig = tlrender::renderFrameAt(timeline, qint64(g9t * 1000000.0), size);
    timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{remapWig}},
        QVector<QVector<ClipInfo>>{}, g9t, -1.0, -1.0, 100);
    timeline->setRotoEntries(QHash<QString, RotoClipEntry>{});
    const QImage remapExpected = tlrender::renderFrameAt(timeline, qint64(g9t * 1000000.0), size);
    const QImage remapSpecial = window.buildSpecialClipComposite(g9t);
    // The GL framebuffer grab is compared inside the band the wiggle shifts: that edge band is
    // transparent in the export and the GL clear grey on screen. Band = ceil(max |baked offset| px) + 1.
    const clipgeom::ClipTransform g9RemapEnv =
        clipanim::effectiveTransformAt(remapWig, g9t, tlrender::dynamicEvalEnv(timeline));
    const clipgeom::ClipTransform g9RemapKf = clipanim::effectiveTransformAt(remapWig, g9t);
    const double g9ShiftPx = std::max(std::abs(g9RemapEnv.videoDx - g9RemapKf.videoDx) * size.width(),
        std::abs(g9RemapEnv.videoDy - g9RemapKf.videoDy) * size.height());
    const int g9Band = int(std::ceil(g9ShiftPx)) + 1;
    const QRect g9Inner(g9Band, g9Band, kW - 2 * g9Band, kH - 2 * g9Band);
    int g9GlInner = 255;
    double g9BakedDx = 1.0, g9BakedDy = 1.0;
    bool g9WOk = false, g9WBaked = false;
    DeviceGrab g9Grab;
    {
        VideoPlayer *wp = window.m_player;
        GLPreview *wgl = wp ? wp->glPreview() : nullptr;
        if (wp && wgl) {
            wgl->setFixedSize(kW, kH);
            wgl->setFrameTextureNearestForTest(true);
            window.show();
            for (int i = 0; i < 8 && !g9WOk; ++i) {
                QApplication::processEvents(QEventLoop::AllEvents, 25);
                const QImage probe = wgl->grabFramebuffer();
                g9WOk = wgl->context() && wgl->context()->isValid() && !probe.isNull();
            }
            bool seeked = false;
            const auto conn = QObject::connect(wp, &VideoPlayer::positionChanged, wp,
                [&](double pos) { if (std::abs(pos - g9t) < 0.001) seeked = true; });
            wp->seek(int(g9t * 1000) + 1);
            wp->seek(int(g9t * 1000));
            (void)QTest::qWaitFor([&] { return seeked; }, 15000);
            QObject::disconnect(conn);
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            window.refreshSpecialClipPreview();
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            const QImage g9GlFrame = g9Grab.toFrame(wgl->grabFramebuffer(), wgl->devicePixelRatioF());
            g9WBaked = wgl->compositeBakedMode();
            g9BakedDx = wgl->videoSourceDx();
            g9BakedDy = wgl->videoSourceDy();
            g9GlInner = rgbMaxDiff(g9GlFrame, remapExpected, g9Inner);
        }
    }
    const DiffStat g9WigVisible = compare(remapExpected, remapNoWig);
    // The upload frame is a CPU QImage (no GL clear): it must equal renderFrameAt over the full frame, RGBA.
    const DiffStat g9SpecialFull = compare(remapSpecial, remapExpected);
    const bool g9ok4 = g9WOk && g9Grab.rawSizeOk && g9WBaked && g9WigVisible.diffPixels > 50
        && g9SpecialFull.sizeMatch && g9SpecialFull.maxDiff == 0 && g9GlInner <= 2 && g9Band > 1
        && g9BakedDx == remapWig.videoDx && g9BakedDy == remapWig.videoDy;

    // (5) arrange writes static values from the keyframes only.
    const auto arrangedDx = [&](bool withWiggle) {
        ClipInfo clip = base;
        clip.videoScale = 0.5;
        clip.videoDx = 0.1;
        if (withWiggle) clip.wiggle = g9Wiggle;
        Timeline tl;
        tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{}, g9t, -1.0, -1.0, 100);
        tl.setProjectOutputConfig(kW, kH, true, kFps);
        tl.setLinkedSelectionEnabled(false);
        tl.clearSelection();
        tl.videoTracks()[0]->toggleClipSelection(0);
        const bool arranged = tl.arrangeSelectedLayers(Timeline::LayerArrangement::Left);
        return std::make_pair(arranged, tl.videoTracks()[0]->clips().value(0).videoDx);
    };
    const auto arrangedWig = arrangedDx(true);
    const auto arrangedPlain = arrangedDx(false);
    const bool g9ok5 = arrangedWig.first && arrangedPlain.first && arrangedWig.second == arrangedPlain.second
        && std::abs(arrangedPlain.second - 0.1) > 1e-6;

    // (6) the project canvas, not the output size, normalizes the pixel amplitude.
    const QSize project1080(1920, 1080), out720(1280, 720);
    clipanim::EvalEnv envOut720;
    envOut720.canvas = out720;
    const clipgeom::ClipTransform g9Exp1080 = clipanim::effectiveTransformAt(g9Clip, g9t, env1080);
    const clipgeom::ClipTransform g9ByOutput = clipanim::effectiveTransformAt(g9Clip, g9t, envOut720);
    const DiffStat g9d6a = compare(renderOne(g9Clip, project1080, g9t, out720),
                                   renderOne(staticHolding(base, g9Exp1080), project1080, g9t, out720));
    const DiffStat g9d6b = compare(renderOne(g9Clip, project1080, g9t, project1080),
                                   renderOne(staticHolding(base, g9Exp1080), project1080, g9t, project1080));
    const DiffStat g9d6c = compare(renderOne(g9Clip, project1080, g9t, out720),
                                   renderOne(staticHolding(base, g9ByOutput), project1080, g9t, out720));
    const bool g9ok6 = g9d6a.sizeMatch && g9d6a.maxDiff == 0 && g9d6b.sizeMatch && g9d6b.maxDiff == 0
        && g9d6c.sizeMatch && g9d6c.diffPixels > 0;

    std::cerr << std::setprecision(12)
              << "special-composite G9 offset t=" << g9t << " px=(" << g9Off.positionOffset.x() << ','
              << g9Off.positionOffset.y() << ") rot=" << g9Off.rotationOffsetDeg << " scale=" << g9Off.scaleMultiplier
              << " sane=" << g9OffSane << '\n'
              << "special-composite G9 (1) wiggle vs none sizeMatch=" << g9d1.sizeMatch << " maxDiff=" << g9d1.maxDiff
              << " diffPixels=" << g9d1.diffPixels << " (expect >50) ok=" << g9ok1 << '\n'
              << "special-composite G9 (2') env1920 dx=" << g9EnvA.videoDx << " expect=" << g9ExpDx
              << " dy=" << g9EnvA.videoDy << " expect=" << g9ExpDy << " twoArg dx=" << g9Two.videoDx
              << " dy=" << g9Two.videoDy << " (expect 0.1 / -0.05) byteEqual="
              << (std::memcmp(&g9EnvA, &g9EnvB, sizeof g9EnvA) == 0) << " ok=" << g9ok2 << '\n'
              << "special-composite G9 (3) envCanvas=" << g9Env.canvas.width() << 'x' << g9Env.canvas.height()
              << " seeked=" << g9Seeked << " glBaked=" << glBaked << " gl local=" << glLocal << " dx=" << glDx
              << " dy=" << glDy << " expect=" << glExp.videoDx << ',' << glExp.videoDy << " | vp recorded=" << vpRecorded
              << " attempts=" << vpAttempts << " local=" << vpLocal << " dx=" << vpDx << " dy=" << vpDy
              << " expect=" << vpExp.videoDx << ',' << vpExp.videoDy << " | renderFrameAt vs static(expected) maxDiff="
              << g9d3.maxDiff << " (expect 0) ok=" << g9ok3 << '\n'
              << "special-composite G9 (4') remap+wiggle windowOk=" << g9WOk << " rawSizeOk=" << g9Grab.rawSizeOk
              << " baked=" << g9WBaked << " shiftPx=" << g9ShiftPx << " band=" << g9Band
              << " glVsExport innerMaxDiff(rgb, band-cropped)=" << g9GlInner
              << " specialVsExport full-frame RGBA sizeMatch=" << g9SpecialFull.sizeMatch
              << " maxDiff=" << g9SpecialFull.maxDiff << " diffPixels=" << g9SpecialFull.diffPixels
              << " (expect 0 / 0) wiggleVisible maxDiff=" << g9WigVisible.maxDiff << " diffPixels="
              << g9WigVisible.diffPixels << " (expect >50) bakedMotion dx=" << g9BakedDx << " dy="
              << g9BakedDy << " (expect keyframes-only " << remapWig.videoDx << ',' << remapWig.videoDy
              << ") ok=" << g9ok4 << '\n'
              << "special-composite G9 (5) arrange Left dx wiggle=" << arrangedWig.second << " (arranged="
              << arrangedWig.first << ") plain=" << arrangedPlain.second << " (arranged=" << arrangedPlain.first
              << ") ok=" << g9ok5 << '\n'
              << "special-composite G9 (6) project1920x1080 out1280x720 vs static(norm 1920) maxDiff=" << g9d6a.maxDiff
              << " out1920x1080 maxDiff=" << g9d6b.maxDiff << " (expect 0 / 0) control static(norm 1280) diffPixels="
              << g9d6c.diffPixels << " (expect >0) dx1920=" << g9Exp1080.videoDx << " dx1280=" << g9ByOutput.videoDx
              << " ok=" << g9ok6 << '\n';
    tally.gate(9, g9ok1 && g9ok2 && g9ok3 && g9ok4 && g9ok5 && g9ok6,
               "the clip wiggle is missing from the export or a preview, applied twice on the baked frame, "
               "written back by arrange, or normalized by the output size (see values above)");

    // G10 (US-1505): the expression bindings are ClipInfo::expressions, evaluated by the shared
    // overlay (keyframes -> autoOrient -> expressions (replace) -> wiggle (add)); audioLevel()
    // reads the Timeline's audio-level snapshot (Timeline::expressionAudioEnvForRead).
    //   (1) renderFrameAt at t = 0.5 with transform.rotation = time*90 differs from without
    //       (HEAD: the export ignores MainWindow's expression sidecar -> equal).
    //   (2) effectiveTransformAt(clip, 0.5, env).rotationDeg == 45 (replaces a base 10).
    //   (3) transform.opacity = 50 -> effectiveOpacityAt == 0.5.
    //   (4) opacity = audioLevel()*100: env.audioLevelAtTime returning 0.5 -> 0.5, no sampler -> 0.
    //   (5) empty bindings: the env form == the two-argument form (keyframed clip, byte-equal).
    //   (6) position.x = 0.5 with the wiggle on: videoDx = 0.5 + wiggle offset (the wiggle after).
    //   (7) time remap + rotation expression: the special composite == renderFrameAt, and the
    //       expression is visible.
    //   (8') a stepped WAV linked to V1 (opacity = audioLevel()*100): relinkMediaPaths, a gap
    //        (ripple delete of an unlinked filler in front), applyTrimLinked (RippleIn, V1 moves
    //        too) and unlinkClipGroupForTest each change the value after the 50 ms debounce (150 ms wait), with a new
    //        snapshot pointer; the old snapshot still gives the old value. Mutation control:
    //        no emit in unlinkClipGroup and no generation compare -> the unlink value stays.
    //   (8'') regression item (HEAD reads the live clips and passes): setClipSpeed(2.0) on the
    //        selected V1[0] changes the linked A1[0] speed (no emit) -> the value changes.
    //        Mutation control: no generation compare -> stale.
    //   (10) a RenderQueue export of the live Timeline: an expression without audioLevel() never
    //        decodes (invocationCountForTest() == 0), one with audioLevel() does (> 0).
    //   (9) a nested sequence's child clip with transform.position.x = audioLevel() and a root A1
    //       of its link group renders at level 0 (the bar stays at x 224); the same clip at the
    //       root moves by the level (bar gone). Position, not opacity: a lone V1 layer is drawn
    //       without its opacity (HEAD's single-layer path).
    //       Mutation control: the depth check removed -> the child reads the root audio.
    std::cerr << "special-composite G10 begin\n";
    const auto g10Wav = [&](const QString &name, double silentSec) {
        const QString wavPath = dir.filePath(name);
        constexpr int g10Sr = 48000;
        QByteArray pcm;
        pcm.reserve(g10Sr * 2 * 2);
        for (int i = 0; i < g10Sr * 2; ++i) {
            const bool loud = silentSec >= 0.0 && i >= int(silentSec * g10Sr);
            pcm.append(char(0x00));
            pcm.append(loud ? char(0x40) : char(0x00));
        }
        QString err;
        return libavcore::writePcm16AsWav(wavPath, pcm, g10Sr, 1, &err) ? wavPath : QString();
    };
    const QString steppedWav = g10Wav(QStringLiteral("g10-stepped.wav"), 0.5); // silent [0, 0.5)
    const QString loudWav = g10Wav(QStringLiteral("g10-loud.wav"), 0.0);
    const QString silentWav = g10Wav(QStringLiteral("g10-silent.wav"), -1.0);
    const bool g10Fixture = !steppedWav.isEmpty() && !loudWav.isEmpty() && !silentWav.isEmpty();
    const auto audioClip = [](const QString &wavPath, double duration, int linkGroup) {
        ClipInfo clip;
        clip.filePath = wavPath;
        clip.duration = clip.outPoint = duration;
        clip.speed = 1.0;
        clip.linkGroup = linkGroup;
        return clip;
    };
    const double g10t = 0.5;

    // (1)
    ClipInfo rotClip = base;
    rotClip.expressions.setExpression(QStringLiteral("transform.rotation"), QStringLiteral("time*90"));
    const DiffStat g10d1 = compare(renderOne(rotClip, size, g10t, size), renderOne(base, size, g10t, size));
    const bool g10ok1 = g10d1.sizeMatch && g10d1.diffPixels > 50;

    // (2) / (3) / (4) / (5) / (6): the shared evaluator.
    clipanim::EvalEnv g10Env;
    g10Env.fps = kFps;
    g10Env.canvas = size;
    ClipInfo rotBased = rotClip;
    rotBased.rotation2DDegrees = 10.0;
    const double g10Rot = clipanim::effectiveTransformAt(rotBased, g10t, g10Env).rotationDeg;
    const bool g10ok2 = std::abs(g10Rot - 45.0) <= 1e-9
        && clipanim::effectiveTransformAt(rotBased, g10t).rotationDeg == 10.0;
    ClipInfo opClip = base;
    opClip.expressions.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("50"));
    const double g10Op = clipanim::effectiveOpacityAt(opClip, g10t, opClip.opacity, g10Env);
    const bool g10ok3 = std::abs(g10Op - 0.5) <= 1e-12;
    ClipInfo levelClip = base;
    levelClip.expressions.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("audioLevel()*100"));
    clipanim::EvalEnv g10EnvLevel = g10Env;
    g10EnvLevel.audioLevelAtTime = [](double) { return 0.5; };
    const double g10OpLevel = clipanim::effectiveOpacityAt(levelClip, g10t, levelClip.opacity, g10EnvLevel);
    const double g10OpNoSampler = clipanim::effectiveOpacityAt(levelClip, g10t, levelClip.opacity, g10Env);
    const bool g10ok4 = std::abs(g10OpLevel - 0.5) <= 1e-12 && g10OpNoSampler == 0.0;
    ClipInfo keyed = base;
    keyed.keyframes.addTrack(audiokf::propertyTrack(QStringLiteral("transform.position.x"),
        {KeyframePoint{0.0, 0.0}, KeyframePoint{1.0, 0.4}}, 0.0));
    const clipgeom::ClipTransform g10Two = clipanim::effectiveTransformAt(keyed, g10t);
    const clipgeom::ClipTransform g10EnvForm = clipanim::effectiveTransformAt(keyed, g10t, g10EnvLevel);
    const bool g10KeyedDynamic = clipanim::hasAnyDynamicTransform(keyed) && keyed.expressions.isEmpty();
    const bool g10ok5 = g10KeyedDynamic && std::memcmp(&g10Two, &g10EnvForm, sizeof g10Two) == 0
        && std::abs(g10Two.videoDx - 0.2) <= 1e-9
        && clipanim::effectiveOpacityAt(keyed, g10t, 0.7, g10EnvLevel)
            == clipanim::effectiveOpacityAt(keyed, g10t, 0.7);
    ClipInfo posWig = base;
    posWig.expressions.setExpression(QStringLiteral("transform.position.x"), QStringLiteral("0.5"));
    posWig.wiggle = g9Wiggle;
    const double g10PosX = clipanim::effectiveTransformAt(posWig, g10t, env1080).videoDx;
    const double g10PosExp = 0.5 + g9Off.positionOffset.x() / 1920.0;
    const bool g10ok6 = g9OffSane && std::abs(g10PosX - g10PosExp) <= 1e-9 && std::abs(g10PosX - 0.5) > 1e-4;

    // (7) time remap + expression through the special composite.
    ClipInfo remapRot = remapped;
    remapRot.expressions.setExpression(QStringLiteral("transform.rotation"), QStringLiteral("time*90"));
    timeline->setRotoEntries(QHash<QString, RotoClipEntry>{});
    timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{remapped}},
        QVector<QVector<ClipInfo>>{}, g10t, -1.0, -1.0, 100);
    const QImage g10RemapPlain = tlrender::renderFrameAt(timeline, qint64(g10t * 1000000.0), size);
    timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{remapRot}},
        QVector<QVector<ClipInfo>>{}, g10t, -1.0, -1.0, 100);
    const QImage g10RemapExport = tlrender::renderFrameAt(timeline, qint64(g10t * 1000000.0), size);
    const QImage g10RemapSpecial = window.buildSpecialClipComposite(g10t);
    const DiffStat g10d7 = compare(g10RemapSpecial, g10RemapExport);
    const DiffStat g10d7v = compare(g10RemapExport, g10RemapPlain);
    const bool g10ok7 = !g10RemapSpecial.isNull() && g10d7.sizeMatch && g10d7.maxDiff <= 2
        && g10d7v.diffPixels > 50;

    // (8') / (8''): the snapshot follows the audio edits.
    ClipInfo linkedV = base;
    linkedV.linkGroup = 9;
    linkedV.expressions.setExpression(QStringLiteral("transform.opacity"), QStringLiteral("audioLevel()*100"));
    const auto levelNow = [&](double local) {
        const TimelineTrack *v1 = timeline->videoTracks().value(0);
        if (!v1 || v1->clipCount() == 0) return -1.0;
        const ClipInfo v = v1->clips().at(0);
        clipanim::EvalEnv env = tlrender::dynamicEvalEnv(timeline);
        env.audioLevelAtTime = timeline->expressionAudioLevelSampler(v, qMax(0.0, v.leadInSec));
        return clipanim::effectiveOpacityAt(v, local, v.opacity, env);
    };
    struct SnapStep {
        bool edited = false;
        double before = -1.0, after = -1.0, oldHolder = -2.0;
        bool ptrChanged = false;
        bool ok() const
        {
            return edited && std::abs(after - before) > 0.2 && ptrChanged && std::abs(oldHolder - before) <= 1e-9;
        }
    };
    const auto runStep = [&](const QVector<ClipInfo> &a1, double local, const std::function<bool()> &edit) {
        SnapStep step;
        timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{linkedV}}, QVector<QVector<ClipInfo>>{a1},
            0.0, -1.0, -1.0, 100);
        // The setup's own debounced (50 ms) sequence emit lands before the edit; 150 ms
        // leaves room for the Windows timer granularity (a 60 ms wait let it land after).
        QTest::qWait(150);
        step.before = levelNow(local);
        const auto envBefore = timeline->expressionAudioEnvForRead();
        const ClipInfo vBefore = timeline->videoTracks().value(0)->clips().value(0);
        step.edited = edit();
        QTest::qWait(150); // past the 50 ms debounce of scheduleEmitSequenceChanged
        QApplication::processEvents();
        step.after = levelNow(local);
        const auto envAfter = timeline->expressionAudioEnvForRead();
        step.ptrChanged = envBefore && envAfter && envBefore.get() != envAfter.get();
        if (envBefore) {
            clipanim::EvalEnv env = tlrender::dynamicEvalEnv(timeline);
            env.audioLevelAtTime = audiokf::makeLinkedAudioSamplerFrom(*envBefore, vBefore.linkGroup,
                                                                       qMax(0.0, vBefore.leadInSec));
            step.oldHolder = clipanim::effectiveOpacityAt(vBefore, local, vBefore.opacity, env);
        }
        return step;
    };
    QString g10Err;
    const SnapStep relinkStep = runStep({audioClip(steppedWav, 2.0, 9)}, 0.75, [&] {
        return timeline->relinkMediaPaths(QHash<QString, QString>{{steppedWav, silentWav}}, &g10Err);
    });
    ClipInfo filler = audioClip(silentWav, 0.5, 0);
    const SnapStep gapStep = runStep({filler, audioClip(steppedWav, 2.0, 9)}, 0.75, [&] {
        return timeline->deleteClipByIndex(true, 0, 0, true, &g10Err);
    });
    const SnapStep trimStep = runStep({audioClip(steppedWav, 2.0, 9)}, 0.25, [&] {
        return timeline->applyTrimLinked(timeline->trackAt(true, 0), 0, trimops::TrimType::RippleIn, 0.5, &g10Err);
    });
    const SnapStep unlinkStep = runStep({audioClip(steppedWav, 2.0, 9)}, 0.75, [&] {
        timeline->unlinkClipGroupForTest(9);
        return timeline->videoTracks().value(0)->clips().value(0).linkGroup == 0;
    });
    const SnapStep speedStep = runStep({audioClip(steppedWav, 2.0, 9)}, 0.4, [&] {
        timeline->videoTracks().value(0)->setSelectedClip(0);
        timeline->setClipSpeed(2.0);
        return timeline->audioTracks().value(0)->clips().value(0).speed == 2.0;
    });
    const bool g10ok8 = g10Fixture && relinkStep.ok() && gapStep.ok() && trimStep.ok() && unlinkStep.ok()
        && std::abs(unlinkStep.after) <= 1e-12 && std::abs(relinkStep.before - 0.5) < 0.05;
    const bool g10ok8b = g10Fixture && speedStep.ok();

    // (10) RenderQueue export of the live Timeline (render thread: atomic_load of the snapshot).
    const auto exportLive = [&](const QString &expression, const QString &outName, QString *err) {
        ClipInfo v = linkedV;
        v.expressions = exprbind::ClipExpressionBindings{};
        v.expressions.setExpression(QStringLiteral("transform.opacity"), expression);
        timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{v}},
            QVector<QVector<ClipInfo>>{{audioClip(loudWav, 2.0, 9)}}, 0.0, -1.0, -1.0, 100);
        QTest::qWait(150);
        audiokf::resetInvocationCountForTest();
        const QString outPath = dir.filePath(outName);
        RenderPreset preset{QStringLiteral("G10"), kW, kH, QStringLiteral("mpeg4"), 600000, QStringLiteral("mp4")};
        RenderJob job = RenderQueue::jobFromPreset(preset, outPath, 0, 500000);
        job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        job.timeline = timeline;
        job.exportConfig["fps"] = kFps;
        job.exportConfig["videoCodec"] = QStringLiteral("mpeg4");
        RenderQueue queue;
        QEventLoop loop;
        bool done = false, success = false;
        QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                         [&](const QString &uuid, bool okJob, const QString &message) {
                             if (uuid != job.uuid) return;
                             done = true;
                             success = okJob;
                             *err = message;
                             loop.quit();
                         });
        queue.addJob(job);
        queue.start();
        if (!done) loop.exec();
        return std::make_pair(success, audiokf::invocationCountForTest());
    };
    QString g10NoLevelErr, g10LevelErr;
    const auto g10NoLevel = exportLive(QStringLiteral("time*100"), QStringLiteral("g10-nolevel.mp4"), &g10NoLevelErr);
    const auto g10WithLevel = exportLive(QStringLiteral("audioLevel()*100"), QStringLiteral("g10-level.mp4"), &g10LevelErr);
    const bool g10ok10 = g10Fixture && g10NoLevel.first && g10NoLevel.second == 0
        && g10WithLevel.first && g10WithLevel.second > 0;

    // (9) nested: the child clip reads level 0; the same clip at the root does not.
    const auto barLevel = [](const QImage &img) {
        if (img.isNull()) return -1;
        const QRgb p = img.convertToFormat(QImage::Format_ARGB32).pixel(224, kH / 2);
        return qRed(p) * qAlpha(p) / 255;
    };
    ClipInfo levelPosV = base;
    levelPosV.linkGroup = 9;
    levelPosV.expressions.setExpression(QStringLiteral("transform.position.x"), QStringLiteral("audioLevel()"));
    timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{levelPosV}},
        QVector<QVector<ClipInfo>>{{audioClip(loudWav, 2.0, 9)}}, 0.0, -1.0, -1.0, 100);
    QTest::qWait(150);
    const int g10RootBar = barLevel(tlrender::renderFrameAt(timeline, qint64(g10t * 1000000.0), size));
    TimelineSequence g10Child;
    g10Child.id = g10Child.name = QStringLiteral("g10-child");
    g10Child.videoTracks = {{levelPosV}};
    g10Child.audioTracks = {{}};
    ClipInfo g10Nested = base;
    g10Nested.filePath = timeline_nesting::sequenceClipFilePath(g10Child.id);
    g10Nested.sequenceRefId = g10Child.id;
    TimelineSequence g10Main;
    g10Main.id = g10Main.name = QStringLiteral("g10-main");
    g10Main.videoTracks = {{g10Nested}};
    g10Main.audioTracks = {{audioClip(loudWav, 2.0, 9)}};
    timeline->setSequences({g10Main, g10Child}, g10Main.id);
    QTest::qWait(150);
    const bool g10NestedActive = timeline->videoTracks().value(0)
        && timeline->videoTracks().value(0)->clipCount() == 1
        && timeline->videoTracks().value(0)->clips().at(0).isSequenceReference()
        && timeline->audioTracks().value(0) && timeline->audioTracks().value(0)->clipCount() == 1;
    const int g10NestedBar = barLevel(tlrender::renderFrameAt(timeline, qint64(g10t * 1000000.0), size));
    const bool g10ok9 = g10Fixture && g10NestedActive && g10RootBar >= 0 && g10RootBar < 20 && g10NestedBar > 200;

    const auto stepText = [](const SnapStep &s) {
        std::ostringstream out;
        out << "edited=" << s.edited << " before=" << s.before << " after=" << s.after
            << " ptrChanged=" << s.ptrChanged << " oldHolder=" << s.oldHolder << " ok=" << s.ok();
        return out.str();
    };
    std::cerr << std::setprecision(12)
              << "special-composite G10 (1) rotation expr vs none sizeMatch=" << g10d1.sizeMatch << " maxDiff="
              << g10d1.maxDiff << " diffPixels=" << g10d1.diffPixels << " (expect >50) ok=" << g10ok1 << '\n'
              << "special-composite G10 (2) rotationDeg=" << g10Rot << " (expect 45, base 10) ok=" << g10ok2 << '\n'
              << "special-composite G10 (3) opacity=" << g10Op << " (expect 0.5) ok=" << g10ok3 << '\n'
              << "special-composite G10 (4) audioLevel opacity sampler0.5=" << g10OpLevel << " none="
              << g10OpNoSampler << " (expect 0.5 / 0) ok=" << g10ok4 << '\n'
              << "special-composite G10 (5) keyed dynamic=" << g10KeyedDynamic << " twoArg dx=" << g10Two.videoDx
              << " env dx=" << g10EnvForm.videoDx << " byteEqual="
              << (std::memcmp(&g10Two, &g10EnvForm, sizeof g10Two) == 0) << " ok=" << g10ok5 << '\n'
              << "special-composite G10 (6) position.x=0.5 + wiggle dx=" << g10PosX << " expect=" << g10PosExp
              << " ok=" << g10ok6 << '\n'
              << "special-composite G10 (7) remap+expr special vs export sizeMatch=" << g10d7.sizeMatch
              << " maxDiff=" << g10d7.maxDiff << " (expect <=2) expr visible diffPixels=" << g10d7v.diffPixels
              << " (expect >50) ok=" << g10ok7 << '\n'
              << "special-composite G10 (8') fixture=" << g10Fixture << " err=[" << g10Err.toStdString() << "]\n"
              << "special-composite G10 (8') relink " << stepText(relinkStep) << '\n'
              << "special-composite G10 (8') gap " << stepText(gapStep) << '\n'
              << "special-composite G10 (8') trimLinked " << stepText(trimStep) << '\n'
              << "special-composite G10 (8') unlink " << stepText(unlinkStep) << " (expect after 0) ok=" << g10ok8 << '\n'
              << "special-composite G10 (8'') setClipSpeed " << stepText(speedStep) << " ok=" << g10ok8b << '\n'
              << "special-composite G10 (10) export no audioLevel ok=" << g10NoLevel.first << " ["
              << g10NoLevelErr.toStdString() << "] invocations=" << g10NoLevel.second << " (expect 0) | audioLevel ok="
              << g10WithLevel.first << " [" << g10LevelErr.toStdString() << "] invocations=" << g10WithLevel.second
              << " (expect >0) ok=" << g10ok10 << '\n'
              << "special-composite G10 (9) root bar@224=" << g10RootBar << " (expect <20, moved by the level) nested active="
              << g10NestedActive << " nested child bar@224=" << g10NestedBar << " (expect >200, level 0) ok=" << g10ok9 << '\n';
    tally.gate(10, g10ok1 && g10ok2 && g10ok3 && g10ok4 && g10ok5 && g10ok6 && g10ok7 && g10ok8 && g10ok8b
                   && g10ok9 && g10ok10,
               "the clip expressions are missing from the export / special composite, not replacing / ordered "
               "before the wiggle, the audio-level snapshot did not follow an edit, a nested clip read the root "
               "audio, or an expression without audioLevel() decoded (see values above)");

    return tally.finish("special-composite");
}
