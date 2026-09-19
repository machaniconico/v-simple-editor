#include "../PuppetPinTool.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"

#include <QTemporaryDir>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace {
bool writeSyntheticClip(const QString &path, const QImage &frame)
{
    constexpr int fps = 10;
    libavcore::EncodeRequest request;
    request.width = frame.width();
    request.height = frame.height();
    request.fps = request.fpsNum = fps;
    request.fpsDen = 1;
    request.videoBitrateBits = 2000000;
    request.outputPath = path.toStdString();
    // Match ClipLutSelftest's supported software codec: acceptance libav
    // builds need not contain a PNG decoder. Preview uses the same decoded frame.
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto error = encoder.open(request)) {
        std::cerr << "puppet-pin fixture open: " << *error << '\n';
        return false;
    }
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    for (int i = 0; i < fps; ++i) {
        if (!encoder.pushFrameRgb24(rgb.constBits(), rgb.bytesPerLine(), i)) {
            std::cerr << "puppet-pin fixture encode failed at frame " << i << '\n';
            return false;
        }
    }
    if (const auto error = encoder.finalize()) {
        std::cerr << "puppet-pin fixture finalize: " << *error << '\n';
        return false;
    }
    return true;
}

bool identical(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()
        || a.format() != b.format() || a.bytesPerLine() != b.bytesPerLine())
        return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(a.bytesPerLine())) != 0)
            return false;
    return true;
}

QImage gradient(QSize size)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, QColor(x * 255 / size.width(), y * 255 / size.height(), 80));
    return image;
}
}

int runPuppetPinSelftest()
{
    int pass = 0, fail = 0;
    const auto gate = [&](int n, bool ok) {
        std::cerr << (ok ? "PASS G" : "FAIL G") << n << '\n';
        ok ? ++pass : ++fail;
    };
    QTemporaryDir directory;
    const QSize size(320, 180);
    const QString path = directory.filePath(QStringLiteral("puppet.mp4"));
    const bool fixture = directory.isValid() && writeSyntheticClip(path, gradient(size));
    const QImage decoded = fixture ? tlrender::detail::decodeClipFrameNativeForTest(path, 0.0) : QImage();
    ClipInfo clip;
    clip.filePath = path;
    clip.duration = clip.outPoint = 1.0;
    Timeline timeline;
    const auto restore = [&](const ClipInfo &value) {
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{value}},
            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    };
    restore(clip);
    const auto render = [&] { return tlrender::renderFrameAt(&timeline, 0, size); };
    ProjectData project;
    project.videoTracks = timeline.allVideoTracks();
    const QString legacy = ProjectFile::toJsonString(project);
    puppetpin::invocationCountForTesting.store(0);
    puppetpin::disabledForTesting.store(true);
    const QImage bypass = render();
    puppetpin::disabledForTesting.store(false);
    const QImage plain = render();
    gate(1, fixture && !decoded.isNull() && identical(bypass, plain)
        && puppetpin::invocationCountForTesting.load() == 0
        && !legacy.contains(QStringLiteral("\"puppetPins\"")) && !clip.hasPuppetPins());

    WarpPin pin;
    pin.originalPos = QPointF(0.5, 0.5);
    pin.deformedPos = QPointF(0.6, 0.55);
    pin.radius = 0.4;
    pin.stiffness = 0.7;
    const QVector<WarpPin> pins{pin};
    timeline.setClipPuppetPins(0, 0, pins);
    const QImage warped = render();
    ClipInfo background;
    background.opacity = 0.0;
    const ClipInfo active = timeline.videoTracks()[0]->clips()[0];
    const QImage preview = VideoPlayer::composeCpuPreviewForTest(
        decoded, active, {}, decoded, background, {}, 0.0, 0.0, size);
    const QImage small = gradient(QSize(640, 360));
    const QImage large = gradient(QSize(1920, 1080));
    const QImage smallWarp = puppetpin::applyNormalized(small, pins);
    const QImage largeWarp = puppetpin::applyNormalized(large, pins);
    bool scaleOk = !identical(small, smallWarp) && !identical(large, largeWarp);
    // Corresponding UV samples, both within and outside the influence circle.
    for (int y = 20; y < 340; y += 20)
        for (int x = 20; x < 620; x += 20) {
            const QColor a = smallWarp.pixelColor(x, y);
            const QColor b = largeWarp.pixelColor(x * 3, y * 3);
            scaleOk &= std::abs(a.red() - b.red()) <= 2
                && std::abs(a.green() - b.green()) <= 2 && a.alpha() == b.alpha();
        }
    gate(2, active.hasPuppetPins() && !identical(plain, warped)
        && identical(preview, warped) && scaleOk);

    project.videoTracks = timeline.allVideoTracks();
    ProjectData loaded, old;
    const auto matches = [&](const ProjectData &data) {
        return !data.videoTracks.isEmpty() && !data.videoTracks[0].isEmpty()
            && puppetpin::equal(data.videoTracks[0][0].puppetPins, pins);
    };
    const QString savedPath = directory.filePath(QStringLiteral("puppet.veditor"));
    gate(3, ProjectFile::fromJsonString(ProjectFile::toJsonString(project), loaded) && matches(loaded)
        && ProjectFile::save(savedPath, project) && ProjectFile::load(savedPath, loaded) && matches(loaded)
        && ProjectFile::fromJsonString(legacy, old) && !old.videoTracks.isEmpty()
        && !old.videoTracks[0].isEmpty() && old.videoTracks[0][0].puppetPins.isEmpty());

    restore(clip);
    PuppetPinTool tool(&timeline, 0, 0);
    const QRectF view(10, 20, 640, 360);
    tool.setViewRect(view);
    SurfaceTool *dispatch = &tool;
    const QPoint origin(330, 200), destination(394, 218);
    const quint64 serial = timeline.undoManager()->saveSerial();
    bool dragOk = dispatch->handleMousePress(origin, Qt::LeftButton, Qt::NoModifier)
        && dispatch->handleMouseMove(destination, Qt::NoModifier)
        && timeline.undoManager()->saveSerial() == serial
        && dispatch->handleMouseRelease(destination, Qt::LeftButton, Qt::NoModifier)
        && timeline.undoManager()->saveSerial() == serial + 1
        && dispatch->hitTestCorner(destination, view) == 0;
    const auto moved = timeline.videoTracks()[0]->clips()[0].puppetPins;
    dragOk &= moved.size() == 1;
    if (moved.size() == 1)
        dragOk &= std::abs(moved[0].deformedPos.x() - 0.6) < 1e-9;
    timeline.undo();
    dragOk &= timeline.videoTracks()[0]->clips()[0].puppetPins.isEmpty();
    timeline.redo();
    dragOk &= puppetpin::equal(moved, timeline.videoTracks()[0]->clips()[0].puppetPins);
    // Moving an existing pin is also one operation, with cancellation non-undoable.
    const quint64 moveSerial = timeline.undoManager()->saveSerial();
    dragOk &= dispatch->handleMousePress(destination, Qt::LeftButton, Qt::NoModifier)
        && dispatch->handleMouseMove(destination + QPoint(20, 0), Qt::NoModifier)
        && timeline.undoManager()->saveSerial() == moveSerial
        && dispatch->handleMouseRelease(destination + QPoint(20, 0), Qt::LeftButton, Qt::NoModifier)
        && timeline.undoManager()->saveSerial() == moveSerial + 1;
    timeline.undo();
    dragOk &= puppetpin::equal(moved, timeline.videoTracks()[0]->clips()[0].puppetPins);
    dragOk &= dispatch->handleMousePress(destination, Qt::LeftButton, Qt::NoModifier)
        && dispatch->handleMouseMove(destination + QPoint(10, 0), Qt::NoModifier);
    tool.cancelDrag();
    dragOk &= puppetpin::equal(moved, timeline.videoTracks()[0]->clips()[0].puppetPins);
    for (bool changePath : {true, false}) {
        restore(active);
        PuppetPinTool stale(&timeline, 0, 0);
        stale.setViewRect(view);
        dragOk &= stale.handleMousePress(destination, Qt::LeftButton, Qt::NoModifier)
            && stale.handleMouseMove(destination + QPoint(20, 0), Qt::NoModifier);
        ClipInfo replacement = timeline.videoTracks()[0]->clips()[0];
        if (changePath) replacement.filePath += QStringLiteral(".replacement");
        else replacement.inPoint += 0.25;
        restore(replacement);
        const quint64 replacementSerial = timeline.undoManager()->saveSerial();
        dragOk &= !stale.isEnabled()
            && !stale.handleMouseMove(origin, Qt::NoModifier);
        stale.handleMouseRelease(origin, Qt::LeftButton, Qt::NoModifier);
        stale.cancelDrag();
        dragOk &= puppetpin::equal(replacement.puppetPins, timeline.videoTracks()[0]->clips()[0].puppetPins)
            && timeline.undoManager()->saveSerial() == replacementSerial;
    }
    gate(4, dragOk);

    restore(active);
    const quint64 resetSerial = timeline.undoManager()->saveSerial();
    timeline.setClipPuppetPins(0, 0, {});
    bool resetOk = timeline.videoTracks()[0]->clips()[0].puppetPins.isEmpty()
        && timeline.undoManager()->saveSerial() == resetSerial + 1 && identical(plain, render());
    timeline.setClipPuppetPins(0, 0, {});
    resetOk &= timeline.undoManager()->saveSerial() == resetSerial + 1;
    project.videoTracks = timeline.allVideoTracks();
    resetOk &= !ProjectFile::toJsonString(project).contains(QStringLiteral("\"puppetPins\""));
    timeline.undo();
    resetOk &= puppetpin::equal(pins, timeline.videoTracks()[0]->clips()[0].puppetPins);
    PuppetPinTool deleting(&timeline, 0, 0);
    deleting.setViewRect(view);
    const quint64 deleteSerial = timeline.undoManager()->saveSerial();
    resetOk &= deleting.handleMousePress(destination, Qt::RightButton, Qt::NoModifier)
        && timeline.undoManager()->saveSerial() == deleteSerial + 1
        && timeline.videoTracks()[0]->clips()[0].puppetPins.isEmpty();
    timeline.undo();
    resetOk &= puppetpin::equal(pins, timeline.videoTracks()[0]->clips()[0].puppetPins);
    gate(5, resetOk);
    std::cerr << "summary: " << pass << " PASS, " << fail << " FAIL\n";
    return fail;
}
