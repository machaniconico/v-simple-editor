#include "../clipanim/ClipAnim.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoEffect.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstdio>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 48;
constexpr int kFps = 10;
constexpr int kFrameCount = 20;
constexpr double kEps = 1e-6;

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

QImage makePatternFrame()
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (32 + x * 3 + y * 5) & 255;
            const int g = (64 + x * 7 + y * 2) & 255;
            const int b = (96 + x * 11 + y * 13) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto err = encoder.open(req)) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }

    const QImage frame = makePatternFrame();
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }
    if (auto err = encoder.finalize()) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("grade-keyframe");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    return clip;
}

void addLinearTrack(KeyframeManager &manager,
                    const QString &name,
                    double defaultValue,
                    double first,
                    double second)
{
    KeyframeTrack track(name, defaultValue);
    track.addKeyframe(0.0, first);
    track.addKeyframe(1.0, second);
    manager.addTrack(track);
}

KeyframeManager makeGradeKeyframes()
{
    KeyframeManager manager;
    addLinearTrack(manager, QStringLiteral("grade.brightness"), 0.0, 0.0, 40.0);
    addLinearTrack(manager, QStringLiteral("grade.contrast"), 0.0, -20.0, 20.0);
    addLinearTrack(manager, QStringLiteral("grade.saturation"), 0.0, 10.0, 50.0);
    addLinearTrack(manager, QStringLiteral("grade.exposure"), 0.0, -1.0, 1.0);
    addLinearTrack(manager, QStringLiteral("grade.temperature"), 0.0, -40.0, 40.0);
    addLinearTrack(manager, QStringLiteral("grade.liftR"), 0.0, 0.0, 0.2);
    addLinearTrack(manager, QStringLiteral("grade.liftG"), 0.0, -0.2, 0.0);
    addLinearTrack(manager, QStringLiteral("grade.liftB"), 0.0, 0.2, -0.2);
    addLinearTrack(manager, QStringLiteral("grade.gammaR"), 0.0, -0.2, 0.2);
    addLinearTrack(manager, QStringLiteral("grade.gammaG"), 0.0, 0.0, 0.4);
    addLinearTrack(manager, QStringLiteral("grade.gammaB"), 0.0, 0.2, 0.0);
    addLinearTrack(manager, QStringLiteral("grade.gainR"), 0.0, 0.0, 0.2);
    addLinearTrack(manager, QStringLiteral("grade.gainG"), 0.0, -0.2, 0.2);
    addLinearTrack(manager, QStringLiteral("grade.gainB"), 0.0, 0.2, 0.4);
    return manager;
}

void setTimelineClip(Timeline &timeline, const ClipInfo &clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
}

bool sameKeyframePoint(const KeyframePoint &a, const KeyframePoint &b)
{
    return near(a.time, b.time)
        && near(a.value, b.value)
        && a.interpolation == b.interpolation
        && near(a.bezX1, b.bezX1)
        && near(a.bezY1, b.bezY1)
        && near(a.bezX2, b.bezX2)
        && near(a.bezY2, b.bezY2)
        && a.hasSpatialTangent == b.hasSpatialTangent
        && near(a.spatialOutX, b.spatialOutX)
        && near(a.spatialOutY, b.spatialOutY)
        && near(a.spatialInX, b.spatialInX)
        && near(a.spatialInY, b.spatialInY);
}

bool sameKeyframeTrack(const KeyframeTrack &a, const KeyframeTrack &b)
{
    if (a.propertyName() != b.propertyName()
        || !near(a.defaultValue(), b.defaultValue())
        || a.keyframes().size() != b.keyframes().size()) {
        return false;
    }
    for (int i = 0; i < a.keyframes().size(); ++i) {
        if (!sameKeyframePoint(a.keyframes()[i], b.keyframes()[i]))
            return false;
    }
    return true;
}

bool sameKeyframes(const KeyframeManager &a, const KeyframeManager &b)
{
    if (a.tracks().size() != b.tracks().size())
        return false;
    for (const KeyframeTrack &track : a.tracks()) {
        const KeyframeTrack *other = b.track(track.propertyName());
        if (!other || !sameKeyframeTrack(track, *other))
            return false;
    }
    return true;
}

ProjectData makeProjectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    data.audioTracks = QVector<QVector<ClipInfo>>{};
    return data;
}

} // namespace

int runGradeKeyframeSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[grade-keyframe] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }

    const QString clipPath = tempDir.filePath(QStringLiteral("grade_keyframe_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: no grade.* tracks returns the static grade unchanged.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.colorCorrection.brightness = 7.0;
        clip.colorCorrection.contrast = -11.0;
        clip.colorCorrection.saturation = 13.0;
        clip.colorCorrection.exposure = 0.25;
        clip.colorCorrection.temperature = -17.0;

        KeyframeTrack motion(QStringLiteral("motion.scale"), 1.0);
        motion.addKeyframe(0.0, 1.0);
        motion.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(motion);

        const ColorCorrection cc = clipanim::effectiveColorCorrectionAt(clip, 0.5);
        check(1, "no grade tracks == static ColorCorrection",
              near(cc.brightness, clip.colorCorrection.brightness)
                  && near(cc.contrast, clip.colorCorrection.contrast)
                  && near(cc.saturation, clip.colorCorrection.saturation)
                  && near(cc.exposure, clip.colorCorrection.exposure)
                  && near(cc.temperature, clip.colorCorrection.temperature));
    }

    // G2: all supported grade.<param> tracks interpolate through clipanim.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.keyframes = makeGradeKeyframes();

        const ColorCorrection cc = clipanim::effectiveColorCorrectionAt(clip, 0.5);
        check(2, "2KF interpolation covers primary + LGG params",
              near(cc.brightness, 20.0)
                  && near(cc.contrast, 0.0)
                  && near(cc.saturation, 30.0)
                  && near(cc.exposure, 0.0)
                  && near(cc.temperature, 0.0)
                  && near(cc.liftR, 0.1)
                  && near(cc.liftG, -0.1)
                  && near(cc.liftB, 0.0)
                  && near(cc.gammaR, 0.0)
                  && near(cc.gammaG, 0.2)
                  && near(cc.gammaB, 0.1)
                  && near(cc.gainR, 0.1)
                  && near(cc.gainG, 0.0)
                  && near(cc.gainB, 0.3),
              QStringLiteral("brightness=%1 saturation=%2 gammaG=%3 gainB=%4")
                  .arg(cc.brightness, 0, 'g', 12)
                  .arg(cc.saturation, 0, 'g', 12)
                  .arg(cc.gammaG, 0, 'g', 12)
                  .arg(cc.gainB, 0, 'g', 12));
    }

    // G3: renderFrameAt samples grade.* just before applyColorCorrection.
    {
        const QSize outSize(80, 60);
        const double sampleSec = 0.5;
        ClipInfo clip = makeClip(clipPath);
        clip.keyframes = makeGradeKeyframes();

        Timeline timeline;
        setTimelineClip(timeline, clip);

        const QImage rendered =
            tlrender::renderFrameAt(&timeline, 500000, outSize);
        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sampleSec);
        const ColorCorrection effective =
            clipanim::effectiveColorCorrectionAt(clip, sampleSec);

        QString detail;
        bool ok = !rendered.isNull() && !native.isNull();
        if (ok) {
            const QImage expected =
                VideoEffectProcessor::applyColorCorrection(native, effective)
                    .convertToFormat(QImage::Format_RGBA8888)
                    .scaled(outSize, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
            const double mse = framediff::mse(rendered, expected);
            const QImage ungraded =
                native.convertToFormat(QImage::Format_RGBA8888)
                    .scaled(outSize, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
            const double changedMse = framediff::mse(rendered, ungraded);
            ok = mse == 0.0 && changedMse > 0.0;
            detail = QStringLiteral("MSE=%1 changedMSE=%2")
                         .arg(mse, 0, 'g', 12)
                         .arg(changedMse, 0, 'g', 12);
        } else {
            detail = rendered.isNull()
                ? QStringLiteral("renderFrameAt returned null")
                : QStringLiteral("decodeClipFrameNativeForTest returned null");
        }
        check(3, "renderFrameAt evaluates grade keyframes", ok, detail);
    }

    // G4: clip with no grade tracks stays byte-identical to the ungraded path.
    {
        const QSize outSize(80, 60);
        const double sampleSec = 0.5;
        ClipInfo clip = makeClip(clipPath);

        Timeline timeline;
        setTimelineClip(timeline, clip);

        const QImage rendered =
            tlrender::renderFrameAt(&timeline, 500000, outSize);
        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sampleSec);
        bool ok = !rendered.isNull() && !native.isNull();
        QString detail;
        if (ok) {
            const QImage expected =
                native.convertToFormat(QImage::Format_RGBA8888)
                    .scaled(outSize, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
            const double mse = framediff::mse(rendered, expected);
            ok = mse == 0.0;
            detail = QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12);
        } else {
            detail = rendered.isNull()
                ? QStringLiteral("renderFrameAt returned null")
                : QStringLiteral("decodeClipFrameNativeForTest returned null");
        }
        check(4, "no grade tracks render byte-identical", ok, detail);
    }

    // G5: timeline undo restores the pre-grade keyframe model.
    {
        const ClipInfo clip = makeClip(clipPath);
        const KeyframeManager gradeKeyframes = makeGradeKeyframes();
        Timeline timeline;
        setTimelineClip(timeline, clip);
        timeline.videoTracks().first()->setSelectedClip(0);

        UndoManager *undo = timeline.undoManager();
        if (undo)
            undo->saveState(timeline.currentState(),
                            QStringLiteral("grade keyframe baseline"));
        const int beforeIndex = undo ? undo->currentIndex() : -1;

        timeline.setClipKeyframes(gradeKeyframes);
        const int afterIndex = undo ? undo->currentIndex() : -1;
        const auto &editedClips = timeline.videoTracks().first()->clips();
        bool ok = undo
            && afterIndex == beforeIndex + 1
            && !editedClips.isEmpty()
            && sameKeyframes(editedClips.first().keyframes, gradeKeyframes);

        if (timeline.canUndo())
            timeline.undo();
        const auto &restoredClips = timeline.videoTracks().first()->clips();
        ok = ok
            && !restoredClips.isEmpty()
            && restoredClips.first().keyframes.tracks().isEmpty()
            && undo
            && undo->currentIndex() == beforeIndex;

        check(5, "setClipKeyframes undo restores pre-grade tracks",
              ok,
              QStringLiteral("before=%1 after=%2")
                  .arg(beforeIndex)
                  .arg(afterIndex));
    }

    // G6: grade.* keyframes survive KeyframeManager and ProjectFile roundtrip.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.keyframes = makeGradeKeyframes();

        KeyframeManager managerRoundtrip;
        managerRoundtrip.fromJson(clip.keyframes.toJson());

        ProjectData loaded;
        const QString json = ProjectFile::toJsonString(makeProjectWithClip(clip));
        const bool projectLoaded = ProjectFile::fromJsonString(json, loaded);
        const bool hasLoadedClip = projectLoaded
            && !loaded.videoTracks.isEmpty()
            && !loaded.videoTracks.first().isEmpty();
        const bool ok = sameKeyframes(clip.keyframes, managerRoundtrip)
            && hasLoadedClip
            && sameKeyframes(clip.keyframes,
                             loaded.videoTracks.first().first().keyframes);
        check(6, "grade keyframe JSON/project roundtrip",
              ok,
              QStringLiteral("projectLoaded=%1")
                  .arg(projectLoaded ? QStringLiteral("true")
                                     : QStringLiteral("false")));
    }

    std::printf("[grade-keyframe] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
