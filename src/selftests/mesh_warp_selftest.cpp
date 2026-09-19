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
#include <limits>

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
    // builds need not contain a PNG decoder. G3 tolerates MPEG-4 noise.
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto error = encoder.open(request)) {
        std::cerr << "mesh-warp fixture open: " << *error << '\n';
        return false;
    }
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    for (int i = 0; i < fps; ++i) {
        if (!encoder.pushFrameRgb24(rgb.constBits(), rgb.bytesPerLine(), i)) {
            std::cerr << "mesh-warp fixture encode failed at frame " << i << '\n';
            return false;
        }
    }
    if (const auto error = encoder.finalize()) {
        std::cerr << "mesh-warp fixture finalize: " << *error << '\n';
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

bool sameGrid(const MeshGrid &a, const MeshGrid &b)
{
    if (a.rows != b.rows || a.cols != b.cols
        || a.controlPoints.size() != b.controlPoints.size()) return false;
    for (int r = 0; r < a.controlPoints.size(); ++r) {
        if (a.controlPoints[r].size() != b.controlPoints[r].size()) return false;
        for (int c = 0; c < a.controlPoints[r].size(); ++c) {
            const QPointF d = a.controlPoints[r][c] - b.controlPoints[r][c];
            if (std::abs(d.x()) > 1e-9 || std::abs(d.y()) > 1e-9) return false;
        }
    }
    return true;
}

double lineCenter(const QImage &image, int y)
{
    if (image.isNull()) return std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0, weight = 0.0;
    for (int x = 0; x < image.width(); ++x) {
        const double w = qMax(0, image.pixelColor(x, y).red() - 128);
        sum += x * w;
        weight += w;
    }
    return weight > 0 ? sum / weight : std::numeric_limits<double>::quiet_NaN();
}
}

int runMeshWarpSelftest()
{
    int pass = 0, fail = 0;
    const auto gate = [&](int n, bool ok) {
        std::cerr << (ok ? "PASS G" : "FAIL G") << n << '\n';
        ok ? ++pass : ++fail;
    };
    const QSize size(320, 180);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::black);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 222; x <= 226; ++x)
            source.setPixelColor(x, y, Qt::white);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("mesh-source.mp4"));
    const bool fixtureOk = directory.isValid() && writeSyntheticClip(path, source);
    // Feed preview the same native RGBA decode that renderFrameAt uses,
    // including any codec quantization, rather than the pre-encode image.
    const QImage decodedSource = fixtureOk
        ? tlrender::detail::decodeClipFrameNativeForTest(path, 0.0) : QImage();
    const bool decodeOk = !decodedSource.isNull() && decodedSource.size() == size
        && decodedSource.format() == QImage::Format_RGBA8888;
    ClipInfo clip;
    clip.filePath = path;
    clip.duration = clip.outPoint = 1.0;
    Timeline timeline;
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
        QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    auto render = [&] { return tlrender::renderFrameAt(&timeline, 0, size); };
    ProjectData project;
    project.videoTracks = timeline.allVideoTracks();
    const QString legacyJson = ProjectFile::toJsonString(project);
    tlrender::resetMeshWarpInvocationCountForTesting();
    tlrender::setMeshWarpDisabledForTesting(true);
    const QImage bypass = render();
    tlrender::setMeshWarpDisabledForTesting(false);
    const QImage plain = render();
    gate(1, fixtureOk && decodeOk && identical(bypass, plain)
        && tlrender::meshWarpInvocationCountForTesting() == 0
        && !legacyJson.contains(QStringLiteral("\"meshWarp\"")));

    const MeshGrid identity = WarpDistortion::createDefaultMesh(QSize(1, 1), 3, 3);
    clip.meshWarp = identity;
    bool defaults = !clip.hasMeshWarp();
    clip.meshWarp.controlPoints[1][1].rx() += 0.2;
    defaults &= clip.hasMeshWarp();
    clip.meshWarp = identity;
    clip.meshWarp.controlPoints[1][1].rx() += 0.5e-6;
    defaults &= !clip.hasMeshWarp();
    clip.meshWarp.controlPoints[1].clear();
    defaults &= !clip.hasMeshWarp();
    clip.meshWarp = identity;
    clip.meshWarp.controlPoints[0][0].setX(std::numeric_limits<double>::infinity());
    defaults &= !clip.hasMeshWarp();
    gate(2, defaults);

    MeshGrid moved = identity;
    for (auto &row : moved.controlPoints) row[1].rx() += 0.2;
    timeline.setClipMeshWarp(0, 0, moved);
    const QImage warped = render();
    // applyMeshWarp is a destination-to-source sampler: destination x=160
    // now samples source x=224. Positive control offsets move content left.
    bool displaced = tlrender::meshWarpInvocationCountForTesting() == 1;
    for (int y : {20, 90, 160}) {
        const double shift = lineCenter(plain, y) - lineCenter(warped, y);
        displaced &= std::abs(shift - 64.0) <= 8.0
            && std::abs(lineCenter(warped, y) - 160.0) <= 8.0;
    }
    gate(3, displaced);

    project.videoTracks = timeline.allVideoTracks();
    ProjectData restored, legacy;
    const QString json = ProjectFile::toJsonString(project);
    bool persisted = json.contains(QStringLiteral("\"meshWarp\""))
        && ProjectFile::fromJsonString(json, restored)
        && !restored.videoTracks.isEmpty() && !restored.videoTracks[0].isEmpty()
        && sameGrid(moved, restored.videoTracks[0][0].meshWarp)
        && ProjectFile::fromJsonString(legacyJson, legacy)
        && !legacy.videoTracks.isEmpty() && !legacy.videoTracks[0].isEmpty()
        && !legacy.videoTracks[0][0].hasMeshWarp();
    const QString projectPath = directory.filePath(QStringLiteral("mesh.veditor"));
    persisted &= ProjectFile::save(projectPath, project)
        && ProjectFile::load(projectPath, restored)
        && !restored.videoTracks.isEmpty() && !restored.videoTracks[0].isEmpty()
        && sameGrid(moved, restored.videoTracks[0][0].meshWarp);
    gate(4, persisted);

    // Re-establish a baseline for a single user operation.
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{legacy.videoTracks.value(0).value(0)}},
        QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    const quint64 serial = timeline.undoManager()->saveSerial();
    timeline.setClipMeshWarp(0, 0, moved);
    bool undoOk = timeline.undoManager()->saveSerial() == serial + 1;
    timeline.setClipMeshWarp(0, 0, moved);
    undoOk &= timeline.undoManager()->saveSerial() == serial + 1;
    const ClipInfo active = timeline.videoTracks()[0]->clips()[0];
    ClipInfo background;
    background.opacity = 0.0;
    const QImage preview = VideoPlayer::composeCpuPreviewForTest(
        decodedSource, active, {}, decodedSource, background, {}, 0.0, 0.0, size);
    undoOk &= decodeOk && tlrender::hasActiveMeshWarp(active)
        && identical(preview, render());
    timeline.undo();
    undoOk &= !timeline.videoTracks()[0]->clips()[0].hasMeshWarp()
        && identical(plain, render());
    timeline.redo();
    undoOk &= sameGrid(moved, timeline.videoTracks()[0]->clips()[0].meshWarp);
    const quint64 resetSerial = timeline.undoManager()->saveSerial();
    timeline.resetClipMeshWarp(0, 0, 3, 3);
    undoOk &= timeline.undoManager()->saveSerial() == resetSerial + 1
        && !timeline.videoTracks()[0]->clips()[0].hasMeshWarp();
    timeline.resetClipMeshWarp(0, 0, 3, 3);
    undoOk &= timeline.undoManager()->saveSerial() == resetSerial + 1;
    project.videoTracks = timeline.allVideoTracks();
    undoOk &= !ProjectFile::toJsonString(project).contains(QStringLiteral("\"meshWarp\""));
    timeline.undo();
    undoOk &= sameGrid(moved, timeline.videoTracks()[0]->clips()[0].meshWarp);
    gate(5, undoOk);
    tlrender::setMeshWarpDisabledForTesting(false);
    std::cerr << "summary: " << pass << " PASS, " << fail << " FAIL\n";
    return fail;
}
