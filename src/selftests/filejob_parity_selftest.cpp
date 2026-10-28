#include "../Camera3D.h"
#include "../Text3DLayer.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../SequenceExport.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../AudioKeyframes.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"

#include <QColor>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int kFps = 10;

struct SequenceTestGuard {
    const bool previous = sequenceexport::enabledForTest();
    SequenceTestGuard() { sequenceexport::setEnabledForTest(true); }
    ~SequenceTestGuard() { sequenceexport::setEnabledForTest(previous); }
};

// The sequence writer flattens onto black into RGB888 (keepAlpha off); do the
// same to the live frame so both sides are in one format.
QImage flatten(const QImage &image)
{
    QImage opaque(image.size(), QImage::Format_RGB888);
    opaque.fill(Qt::black);
    QPainter painter(&opaque);
    painter.drawImage(0, 0, image);
    painter.end();
    return opaque;
}

// Differing pixel count over the WHOLE image; -1 when either side is null or
// the sizes differ (never a passing comparison).
qint64 differingPixels(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    qint64 differing = 0;
    for (int y = 0; y < left.height(); ++y)
        for (int x = 0; x < left.width(); ++x)
            if (left.pixelColor(x, y) != right.pixelColor(x, y))
                ++differing;
    return differing;
}

// One frame of a file-backed (projectFilePath) RenderQueue job: a one-frame
// PNG sequence export, which goes through RenderQueue::resolveTimeline and
// tlrender::renderFrameAt. A null image means the save, export or decode failed.
QImage renderFileJobFrame(const QDir &dir, const ProjectData &project, const QString &name,
                          double targetSec)
{
    const QString projectPath = dir.filePath(name + QStringLiteral(".veditor"));
    const QString folder = dir.filePath(name);
    if (!ProjectFile::save(projectPath, project))
        return QImage();
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
    renderJob.exportConfig.insert(QStringLiteral("sequencePattern"), QStringLiteral("fj_%04d.png"));
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
    const QImage image(QDir(folder).filePath(QStringLiteral("fj_0000.png")));
    if (!completed || image.isNull() || QDir(folder).entryList(QDir::Files).size() != 1)
        return QImage();
    return image;
}

ClipInfo layerClip(bool autoOrientToCamera)
{
    Shape rectangle;
    rectangle.type = ShapeType::Rectangle;
    rectangle.position = QPointF(kW / 2.0, kH / 2.0);
    rectangle.properties.size = QSizeF(kW * 0.6, kH * 0.6);
    rectangle.fill.color = QColor(220, 80, 40);
    rectangle.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("filejob-parity layer");
    clip.duration = clip.outPoint = 2.0;
    clip.shapes = {rectangle};
    clip.is3DLayer = true;
    clip.layer3D.rotationY = 30.0;
    clip.autoOrientToCamera = autoOrientToCamera;
    return clip;
}

// Animated trueProjection project camera: pans and rolls between 0 s and 1 s.
Camera3D animatedCamera()
{
    Camera3DState start;
    start.trueProjection = true;
    Camera3DState end = start;
    end.position = QVector3D(0.4f, 0.1f, 0.0f);
    end.target = QVector3D(0.3f, 0.0f, -1.0f);
    end.roll = 10.0;
    Camera3D camera{start};
    camera.setCameraKeyframe(0.0, start);
    camera.setCameraKeyframe(1.0, end);
    return camera;
}

ProjectData projectWith(const ClipInfo &clip, const Camera3D &camera)
{
    ProjectData project;
    project.config.width = kW;
    project.config.height = kH;
    project.config.fps = kFps;
    project.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
    project.projectCamera = camera.toJson();
    return project;
}

// The live Timeline MainWindow would hold for the same project: the same clips
// and project camera (MainWindow.cpp setProjectCamera), rendered by renderFrameAt.
QImage liveFrame(const ProjectData &project, const Camera3D &camera, double targetSec)
{
    Timeline timeline;
    timeline.setProjectOutputConfig(project.config.width, project.config.height,
                                    project.config.explicitOutputResolution, project.config.fps);
    timeline.restoreFromProject(project.videoTracks, project.audioTracks, 0.0, -1.0, -1.0, 100);
    timeline.setProjectCamera(camera);
    return flatten(tlrender::renderFrameAt(&timeline, qint64(targetSec * 1000000.0), QSize(kW, kH),
                                           1000000.0 / kFps));
}

// A 2-D V1 layer whose opacity is the expression audioLevel() * 100, linked
// (linkGroup) to an audio clip of audioPath when audioPath is not empty.
ProjectData audioLevelProject(const QString &audioPath)
{
    ClipInfo clip = layerClip(false);
    clip.is3DLayer = false;
    clip.layer3D.rotationY = 0.0;
    clip.linkGroup = 5;
    clip.expressions.setExpression(QStringLiteral("transform.opacity"),
                                   QStringLiteral("audioLevel() * 100"));
    ProjectData project = projectWith(clip, Camera3D{});
    if (!audioPath.isEmpty()) {
        ClipInfo audio;
        audio.duration = audio.outPoint = 2.0;
        audio.linkGroup = 5;
        audio.filePath = audioPath;
        project.audioTracks = QVector<QVector<ClipInfo>>{{audio}};
    }
    return project;
}

} // namespace

int runFileJobParitySelftest()
{
    selftest::GateTally tally;
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        tally.gate(1, false, "temporary directory missing");
        tally.gate(2, false, "temporary directory missing");
        tally.gate(3, false, "temporary directory missing");
        tally.gate(4, false, "temporary directory missing");
        return tally.finish("filejob-parity");
    }
    const QDir dir(tempDir.path());
    const double targetSec = 0.5;
    const Camera3D camera = animatedCamera();

    // G1: trueProjection + animated project camera + a rotY 30 layer: the
    // file-backed job frame == the live frame. Controls: the same live frame
    // WITHOUT the camera (the HEAD file-job behaviour) must differ, so the
    // camera is visible; a failed export is never a passing comparison.
    {
        const ClipInfo clip = layerClip(false);
        const ProjectData project = projectWith(clip, camera);
        const QImage live = liveFrame(project, camera, targetSec);
        const QImage fileJob = renderFileJobFrame(dir, project, QStringLiteral("g1"), targetSec);
        const QImage noCamera = liveFrame(project, Camera3D{}, targetSec);
        const qint64 diff = differingPixels(fileJob, live);
        const qint64 headDiff = differingPixels(noCamera, live);
        std::cerr << "filejob-parity G1 fileJobVsLive=" << diff << " noCameraVsLive=" << headDiff << '\n';
        tally.gate(1, !live.isNull() && !fileJob.isNull() && diff == 0 && headDiff > 0,
                   "file job frame differs from the live frame under the project camera");
    }

    // G2: the same with autoOrientToCamera. Controls: the live frame with
    // auto-orient must differ from the one without it (the orientation is
    // visible), and the no-camera frame must differ from the live frame.
    {
        const ClipInfo clip = layerClip(true);
        const ProjectData project = projectWith(clip, camera);
        const QImage live = liveFrame(project, camera, targetSec);
        const QImage fileJob = renderFileJobFrame(dir, project, QStringLiteral("g2"), targetSec);
        const QImage noCamera = liveFrame(project, Camera3D{}, targetSec);
        const QImage noOrient = liveFrame(projectWith(layerClip(false), camera), camera, targetSec);
        const qint64 diff = differingPixels(fileJob, live);
        const qint64 headDiff = differingPixels(noCamera, live);
        const qint64 orientDiff = differingPixels(noOrient, live);
        std::cerr << "filejob-parity G2 fileJobVsLive=" << diff << " noCameraVsLive=" << headDiff
                  << " noOrientVsLive=" << orientDiff << '\n';
        tally.gate(2, !live.isNull() && !fileJob.isNull() && diff == 0 && headDiff > 0 && orientDiff > 0,
                   "auto-orient file job frame differs from the live frame under the project camera");
    }

    // G3 (US-1709): a V1 layer whose opacity is audioLevel() * 100, linked to a
    // loud (0.5 amplitude) audio clip. The file-backed job frame must differ from
    // the same project without the audio track (HEAD: the owned Timeline has no
    // audio-level snapshot, so audioLevel() is 0 either way and the frames are
    // identical) and equal the live frame whose Timeline publishes the snapshot
    // the way MainWindow does.
    {
        QString error;
        const QString wav = dir.filePath(QStringLiteral("g3-loud.wav"));
        QByteArray pcm;
        for (int i = 0; i < 48000 * 2; ++i) {
            pcm.append(char(0x00));
            pcm.append(char(0x40));
        }
        const bool wavOk = libavcore::writePcm16AsWav(wav, pcm, 48000, 1, &error);
        const ProjectData withAudio = audioLevelProject(wav);
        const ProjectData withoutAudio = audioLevelProject(QString());
        const QImage fileWith = renderFileJobFrame(dir, withAudio, QStringLiteral("g3a"), targetSec);
        const QImage fileWithout = renderFileJobFrame(dir, withoutAudio, QStringLiteral("g3b"), targetSec);
        QImage live;
        {
            Timeline timeline;
            timeline.setProjectOutputConfig(kW, kH, withAudio.config.explicitOutputResolution, kFps);
            timeline.restoreFromProject(withAudio.videoTracks, withAudio.audioTracks, 0.0, -1.0, -1.0, 100);
            timeline.setExpressionAudioEnv(
                timeline.buildExpressionAudioEnv(audiokf::sharedEnvelopeCache()));
            live = flatten(tlrender::renderFrameAt(&timeline, qint64(targetSec * 1000000.0),
                                                   QSize(kW, kH), 1000000.0 / kFps));
        }
        const qint64 audioDiff = differingPixels(fileWith, fileWithout);
        const qint64 liveDiff = differingPixels(fileWith, live);
        std::cerr << "filejob-parity G3 wav=" << wavOk << " withVsWithoutAudio=" << audioDiff
                  << " (HEAD 0) fileJobVsLive=" << liveDiff << '\n';
        tally.gate(3, wavOk && !fileWith.isNull() && !fileWithout.isNull() && !live.isNull()
                          && audioDiff > 0 && liveDiff == 0,
                   "file job audioLevel() opacity ignores the linked audio or differs from the live frame");
    }

    // G4 (US-1707): a 2-D V1 layer carrying a 3D text. The file-backed job frame
    // must differ from the same project without the text (HEAD: renderFrameAt
    // ignored text3DConfig, so the two were identical) and equal the live frame.
    {
        ClipInfo clip = layerClip(false);
        clip.is3DLayer = false;
        clip.layer3D.rotationY = 0.0;
        const ProjectData withoutText = projectWith(clip, Camera3D{});
        Text3DLayer text;
        text.setText(QStringLiteral("FJ4"), QFont(QStringLiteral("Arial"), 72));
        text.setExtrudeEnabled(true);
        text.setExtrude(0.35, 0.03, 0.03, 2);
        text.setCameraDistance(3.0);
        clip.text3DConfig = text.toJson();
        const ProjectData withText = projectWith(clip, Camera3D{});
        const QImage fileWith = renderFileJobFrame(dir, withText, QStringLiteral("g4a"), targetSec);
        const QImage fileWithout = renderFileJobFrame(dir, withoutText, QStringLiteral("g4b"), targetSec);
        const QImage live = liveFrame(withText, Camera3D{}, targetSec);
        const qint64 textDiff = differingPixels(fileWith, fileWithout);
        const qint64 liveDiff = differingPixels(fileWith, live);
        std::cerr << "filejob-parity G4 withVsWithoutText=" << textDiff
                  << " (HEAD 0) fileJobVsLive=" << liveDiff << '\n';
        tally.gate(4, !fileWith.isNull() && !fileWithout.isNull() && !live.isNull()
                          && textDiff > 100 && liveDiff == 0,
                   "file job frame lost the 3D text or differs from the live frame");
    }

    return tally.finish("filejob-parity");
}
