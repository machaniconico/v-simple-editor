// US-708: MainWindow::buildSpecialClipComposite (special-data preview path) must
// run the same native FX stack (mesh warp / puppet pins / FX pack) as the export
// path tlrender::renderFrameAt, so preview and export agree.
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../WarpDistortion.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
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

} // namespace

int runSpecialCompositeSelftest()
{
    selftest::GateTally tally;
    const QSize size(kW, kH);

    QString encoderWhy;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderWhy)) {
        std::cerr << "special-composite: " << encoderWhy.toStdString() << '\n';
        for (int g = 1; g <= 4; ++g)
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

    return tally.finish("special-composite");
}
