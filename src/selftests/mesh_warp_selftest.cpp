#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"

#include <QTemporaryDir>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
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
    const QString path = directory.filePath(QStringLiteral("mesh-source.png"));
    const bool fixtureOk = directory.isValid() && source.save(path);
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
    gate(1, fixtureOk && identical(bypass, plain)
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
        source, active, {}, source, background, {}, 0.0, 0.0, size);
    undoOk &= tlrender::hasActiveMeshWarp(active) && identical(preview, render());
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
