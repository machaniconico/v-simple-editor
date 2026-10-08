#include "../clipanim/ClipAnim.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"
#include "../GLPreview.h"
#include "../GraphEditorPanel.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QSize>
#include <QStringList>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
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

bool writeSyntheticClip(const QString &path, QString *error, int frameCount = kFrameCount)
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
    for (int i = 0; i < frameCount; ++i) {
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

ClipInfo makeClip(const QString &path, int frameCount = kFrameCount)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("grade-keyframe");
    clip.duration = static_cast<double>(frameCount) / kFps;
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

bool sameColorCorrection(const ColorCorrection &a, const ColorCorrection &b)
{
    return near(a.brightness, b.brightness)
        && near(a.contrast, b.contrast)
        && near(a.saturation, b.saturation)
        && near(a.hue, b.hue)
        && near(a.temperature, b.temperature)
        && near(a.tint, b.tint)
        && near(a.gamma, b.gamma)
        && near(a.highlights, b.highlights)
        && near(a.shadows, b.shadows)
        && near(a.exposure, b.exposure)
        && near(a.liftR, b.liftR)
        && near(a.liftG, b.liftG)
        && near(a.liftB, b.liftB)
        && near(a.gammaR, b.gammaR)
        && near(a.gammaG, b.gammaG)
        && near(a.gammaB, b.gammaB)
        && near(a.gainR, b.gainR)
        && near(a.gainG, b.gainG)
        && near(a.gainB, b.gainB);
}

bool glPreviewInitializes(QString *detail)
{
    if (!qApp) {
        if (detail)
            *detail = QStringLiteral("no QApplication instance");
        return false;
    }

    GLPreview probe;
    probe.resize(64, 48);
    probe.show();
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        probe.grabFramebuffer();
        if (probe.context() && probe.context()->isValid())
            return true;
    }

    if (detail)
        *detail = QStringLiteral("no usable QOpenGLWidget context");
    return false;
}

ProjectData makeProjectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    data.audioTracks = QVector<QVector<ClipInfo>>{};
    return data;
}

// Keys for the four US-717 tracks: 0 s -> first, 2 s -> second.
void addTwoSecondTrack(KeyframeManager &manager, const QString &name,
                       double defaultValue, double first, double second)
{
    KeyframeTrack track(name, defaultValue);
    track.addKeyframe(0.0, first);
    track.addKeyframe(2.0, second);
    manager.addTrack(track);
}

QImage renderClipAt(const ClipInfo &clip, double seconds, const QSize &outSize)
{
    Timeline timeline;
    setTimelineClip(timeline, clip);
    return tlrender::renderFrameAt(&timeline,
                                   static_cast<qint64>(std::llround(seconds * 1000000.0)),
                                   outSize);
}

// Largest per-channel difference in 0..255 units; -1 when the images cannot
// be compared.
int maxChannelDiff(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    const QImage ca = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage cb = b.convertToFormat(QImage::Format_RGBA8888);
    int worst = 0;
    for (int y = 0; y < ca.height(); ++y) {
        const uchar *ra = ca.constScanLine(y);
        const uchar *rb = cb.constScanLine(y);
        for (int x = 0; x < ca.width() * 4; ++x)
            worst = std::max(worst, std::abs(int(ra[x]) - int(rb[x])));
    }
    return worst;
}

} // namespace

int runGradeKeyframeSelftest()
{
    selftest::GateTally tally;

    // Each gate prints its composite values before the PASS/FAIL line.
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::cerr << "[grade-keyframe] G" << gate << ' ' << name;
        if (!detail.isEmpty())
            std::cerr << " - " << detailUtf8.constData();
        std::cerr << '\n';
        tally.gate(gate, ok, detailUtf8.constData());
    };
    auto skip = [&](int gate, const char *name, const QString &detail) {
        const QByteArray reason =
            (QString::fromUtf8(name) + QStringLiteral(" - ") + detail).toUtf8();
        tally.skip(gate, reason.constData());
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return tally.finish("grade-keyframe");
    }

    const QString clipPath = tempDir.filePath(QStringLiteral("grade_keyframe_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return tally.finish("grade-keyframe");
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

    // G7: GPU preview pushes the playhead-evaluated grade to GLPreview, while
    // composite-baked mode and disabled GPU effects leave the current GL grade
    // untouched.
    {
        QString glSkipDetail;
        if (!glPreviewInitializes(&glSkipDetail)) {
            skip(7, "GPU preview grade keyframe wiring", glSkipDetail);
        } else {
            ClipInfo clip = makeClip(clipPath);
            clip.keyframes = makeGradeKeyframes();

            Timeline timeline;
            setTimelineClip(timeline, clip);

            PlaybackEntry entry;
            entry.filePath = clipPath;
            entry.clipIn = 0.0;
            entry.clipOut = clip.duration;
            entry.timelineStart = 0.0;
            entry.timelineEnd = clip.duration;
            entry.speed = 1.0;
            entry.sourceTrack = 0;
            entry.sourceClipIndex = 0;

            VideoPlayer player;
            player.resize(160, 120);
            player.glPreview()->setTimeline(&timeline);
            player.glPreview()->setCompositeBakedMode(false);
            player.setSequence(QVector<PlaybackEntry>{entry});

            QSettings prefs("VSimpleEditor", "Preferences");
            const bool hadGpuEffectsPref = prefs.contains("gpuEffectsEnabled");
            const QVariant savedGpuEffectsPref = prefs.value("gpuEffectsEnabled");
            prefs.setValue("gpuEffectsEnabled", true);
            prefs.sync();
            player.setGpuEffectsEnabled(true);

            GLPreview *preview = player.glPreview();
            const int beforeAnimatedPush = preview->colorCorrectionSetCountForTest();
            const bool animatedPushed =
                player.pushActiveClipColorCorrectionToGlPreviewForTest(500000);
            const int afterAnimatedPush = preview->colorCorrectionSetCountForTest();
            const ColorCorrection expectedAnimated =
                clipanim::effectiveColorCorrectionAt(clip, 0.5);
            const bool animatedOk =
                animatedPushed
                && afterAnimatedPush == beforeAnimatedPush + 1
                && sameColorCorrection(preview->colorCorrectionForTest(),
                                       expectedAnimated);

            preview->setCompositeBakedMode(true);
            const int beforeBakedGuard = preview->colorCorrectionSetCountForTest();
            const bool bakedPushed =
                player.pushActiveClipColorCorrectionToGlPreviewForTest(500000);
            const int afterBakedGuard = preview->colorCorrectionSetCountForTest();
            preview->setCompositeBakedMode(false);
            const bool bakedGuardOk =
                !bakedPushed && afterBakedGuard == beforeBakedGuard;

            prefs.setValue("gpuEffectsEnabled", false);
            prefs.sync();
            player.setGpuEffectsEnabled(false);
            const int beforeDisabledGuard = preview->colorCorrectionSetCountForTest();
            const bool disabledPushed =
                player.pushActiveClipColorCorrectionToGlPreviewForTest(500000);
            const int afterDisabledGuard = preview->colorCorrectionSetCountForTest();
            const bool shaderStillEnabled = preview->effectsEnabled();
            prefs.setValue("gpuEffectsEnabled", true);
            prefs.sync();
            player.setGpuEffectsEnabled(true);
            const bool disabledGuardOk =
                !disabledPushed
                && afterDisabledGuard == beforeDisabledGuard
                && shaderStillEnabled;

            ClipInfo staticClip = makeClip(clipPath);
            staticClip.colorCorrection.brightness = 6.0;
            staticClip.colorCorrection.contrast = -4.0;
            staticClip.colorCorrection.saturation = 8.0;
            staticClip.colorCorrection.exposure = 0.125;
            staticClip.colorCorrection.temperature = 12.0;
            staticClip.colorCorrection.liftR = 0.05;
            staticClip.colorCorrection.gammaG = -0.10;
            staticClip.colorCorrection.gainB = 0.15;
            setTimelineClip(timeline, staticClip);

            const int beforeStaticPush = preview->colorCorrectionSetCountForTest();
            const bool staticPushed =
                player.pushActiveClipColorCorrectionToGlPreviewForTest(500000);
            const int afterStaticPush = preview->colorCorrectionSetCountForTest();
            const bool staticNoopOk =
                staticPushed
                && afterStaticPush == beforeStaticPush + 1
                && sameColorCorrection(preview->colorCorrectionForTest(),
                                       staticClip.colorCorrection);

            const bool ok = animatedOk && bakedGuardOk
                && disabledGuardOk && staticNoopOk;
            check(7, "GPU preview grade keyframe wiring",
                  ok,
                  QStringLiteral("animated=%1 bakedGuard=%2 disabledGuard=%3 staticNoop=%4 shaderStillEnabled=%5")
                      .arg(animatedOk ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(bakedGuardOk ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(disabledGuardOk ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(staticNoopOk ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(shaderStillEnabled ? QStringLiteral("true") : QStringLiteral("false")));

            if (hadGpuEffectsPref)
                prefs.setValue("gpuEffectsEnabled", savedGpuEffectsPref);
            else
                prefs.remove("gpuEffectsEnabled");
            prefs.sync();
        }
    }

    // G8..G12 (US-717): grade.hue / grade.gamma / grade.highlights /
    // grade.shadows. The keys run 0 s -> 2 s, so a 3 s clip keeps the 2 s
    // endpoint inside the clip.
    const QString longClipPath = tempDir.filePath(QStringLiteral("grade_keyframe_long_src.mp4"));
    QString longMediaError;
    const bool longClipOk = writeSyntheticClip(longClipPath, &longMediaError, kFrameCount + kFps);
    const int longFrames = kFrameCount + kFps;
    const QSize parityOutSize(80, 60);

    // Renders the keyed clip at 0 / 1 / 2 s and compares each time with a
    // static-grade render (diffs via keyedDiff); returns the 1 s diff against
    // the static default grade.
    struct ParityCase {
        const char *track;
        double first;
        double second;
        double ColorCorrection::*member;
        double staticAt[3];
    };
    auto runParity = [&](const ParityCase &pc, int keyedDiff[3]) {
        ClipInfo keyed = makeClip(longClipPath, longFrames);
        addTwoSecondTrack(keyed.keyframes, QString::fromLatin1(pc.track),
                          keyed.colorCorrection.*(pc.member), pc.first, pc.second);
        for (int i = 0; i < 3; ++i) {
            ClipInfo reference = makeClip(longClipPath, longFrames);
            reference.colorCorrection.*(pc.member) = pc.staticAt[i];
            keyedDiff[i] = maxChannelDiff(renderClipAt(keyed, double(i), parityOutSize),
                                          renderClipAt(reference, double(i), parityOutSize));
        }
        ClipInfo staticDefault = makeClip(longClipPath, longFrames);
        return maxChannelDiff(renderClipAt(keyed, 1.0, parityOutSize),
                              renderClipAt(staticDefault, 1.0, parityOutSize));
    };
    auto parityOk = [](const int keyedDiff[3], int defaultDiff) {
        for (int i = 0; i < 3; ++i) {
            if (keyedDiff[i] < 0 || keyedDiff[i] > 1)
                return false;
        }
        return defaultDiff >= 3;
    };
    auto parityDetail = [](const char *track, const int keyedDiff[3], int defaultDiff) {
        return QStringLiteral("%1: diff@0s=%2 diff@1s=%3 diff@2s=%4 (want <=1) diffVsDefault@1s=%5 (want >=3)")
            .arg(QString::fromLatin1(track))
            .arg(keyedDiff[0])
            .arg(keyedDiff[1])
            .arg(keyedDiff[2])
            .arg(defaultDiff);
    };

    // G8: grade.hue 0 -> +120 matches static hue 0 / 60 / 120 renders.
    if (!longClipOk) {
        check(8, "grade.hue keyframes match static hue renders", false, longMediaError);
    } else {
        const ParityCase hueCase{"grade.hue", 0.0, 120.0, &ColorCorrection::hue,
                                 {0.0, 60.0, 120.0}};
        int diff[3] = {-1, -1, -1};
        const int defaultDiff = runParity(hueCase, diff);
        check(8, "grade.hue keyframes match static hue renders",
              parityOk(diff, defaultDiff),
              parityDetail(hueCase.track, diff, defaultDiff));
    }

    // G9: grade.gamma 1.0 -> 2.2, grade.highlights 0 -> +80,
    // grade.shadows 0 -> -60 each match their static midpoint renders.
    if (!longClipOk) {
        check(9, "grade.gamma/highlights/shadows keyframes match static renders",
              false, longMediaError);
    } else {
        const ParityCase cases[] = {
            {"grade.gamma", 1.0, 2.2, &ColorCorrection::gamma, {1.0, 1.6, 2.2}},
            {"grade.highlights", 0.0, 80.0, &ColorCorrection::highlights, {0.0, 40.0, 80.0}},
            {"grade.shadows", 0.0, -60.0, &ColorCorrection::shadows, {0.0, -30.0, -60.0}},
        };
        bool ok = true;
        QStringList details;
        for (const ParityCase &pc : cases) {
            int diff[3] = {-1, -1, -1};
            const int defaultDiff = runParity(pc, diff);
            ok = parityOk(diff, defaultDiff) && ok;
            details << parityDetail(pc.track, diff, defaultDiff);
        }
        check(9, "grade.gamma/highlights/shadows keyframes match static renders",
              ok, details.join(QStringLiteral("; ")));
    }

    auto makeFourTrackClip = [&]() {
        ClipInfo clip = makeClip(longClipPath, longFrames);
        addTwoSecondTrack(clip.keyframes, QStringLiteral("grade.hue"), 0.0, 0.0, 120.0);
        addTwoSecondTrack(clip.keyframes, QStringLiteral("grade.gamma"), 1.0, 1.0, 2.2);
        addTwoSecondTrack(clip.keyframes, QStringLiteral("grade.highlights"), 0.0, 0.0, 80.0);
        addTwoSecondTrack(clip.keyframes, QStringLiteral("grade.shadows"), 0.0, 0.0, -60.0);
        return clip;
    };

    // G10: the preview pushes the 1 s evaluated hue / gamma / highlights /
    // shadows to GLPreview.
    {
        QString glSkipDetail;
        if (!longClipOk) {
            check(10, "GPU preview receives keyed hue/gamma/highlights/shadows",
                  false, longMediaError);
        } else if (!glPreviewInitializes(&glSkipDetail)) {
            skip(10, "GPU preview receives keyed hue/gamma/highlights/shadows", glSkipDetail);
        } else {
            const ClipInfo clip = makeFourTrackClip();
            Timeline timeline;
            setTimelineClip(timeline, clip);

            PlaybackEntry entry;
            entry.filePath = longClipPath;
            entry.clipIn = 0.0;
            entry.clipOut = clip.duration;
            entry.timelineStart = 0.0;
            entry.timelineEnd = clip.duration;
            entry.speed = 1.0;
            entry.sourceTrack = 0;
            entry.sourceClipIndex = 0;

            VideoPlayer player;
            player.resize(160, 120);
            player.glPreview()->setTimeline(&timeline);
            player.glPreview()->setCompositeBakedMode(false);
            player.setSequence(QVector<PlaybackEntry>{entry});

            QSettings prefs("VSimpleEditor", "Preferences");
            const bool hadGpuEffectsPref = prefs.contains("gpuEffectsEnabled");
            const QVariant savedGpuEffectsPref = prefs.value("gpuEffectsEnabled");
            prefs.setValue("gpuEffectsEnabled", true);
            prefs.sync();
            player.setGpuEffectsEnabled(true);

            GLPreview *preview = player.glPreview();
            const bool pushed =
                player.pushActiveClipColorCorrectionToGlPreviewForTest(1000000);
            const ColorCorrection got = preview->colorCorrectionForTest();
            const ColorCorrection want = clipanim::effectiveColorCorrectionAt(clip, 1.0);
            const ColorCorrection &st = clip.colorCorrection;
            const bool matches = near(got.hue, want.hue, 1e-9)
                && near(got.gamma, want.gamma, 1e-9)
                && near(got.highlights, want.highlights, 1e-9)
                && near(got.shadows, want.shadows, 1e-9);
            const bool differsFromStatic = !near(got.hue, st.hue)
                && !near(got.gamma, st.gamma)
                && !near(got.highlights, st.highlights)
                && !near(got.shadows, st.shadows);
            check(10, "GPU preview receives keyed hue/gamma/highlights/shadows",
                  pushed && matches && differsFromStatic,
                  QStringLiteral("pushed=%1 hue=%2/%3 gamma=%4/%5 highlights=%6/%7 shadows=%8/%9 (got/want) differsFromStatic=%10")
                      .arg(pushed ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(got.hue, 0, 'g', 12).arg(want.hue, 0, 'g', 12)
                      .arg(got.gamma, 0, 'g', 12).arg(want.gamma, 0, 'g', 12)
                      .arg(got.highlights, 0, 'g', 12).arg(want.highlights, 0, 'g', 12)
                      .arg(got.shadows, 0, 'g', 12).arg(want.shadows, 0, 'g', 12)
                      .arg(differsFromStatic ? QStringLiteral("true") : QStringLiteral("false")));

            if (hadGpuEffectsPref)
                prefs.setValue("gpuEffectsEnabled", savedGpuEffectsPref);
            else
                prefs.remove("gpuEffectsEnabled");
            prefs.sync();
        }
    }

    // G11: the four tracks survive save -> load -> save byte-identically.
    {
        const ClipInfo clip = makeFourTrackClip();
        const QString first = ProjectFile::toJsonString(makeProjectWithClip(clip));
        ProjectData loaded;
        const bool projectLoaded = ProjectFile::fromJsonString(first, loaded);
        const bool hasLoadedClip = projectLoaded
            && !loaded.videoTracks.isEmpty()
            && !loaded.videoTracks.first().isEmpty();
        QStringList missing;
        for (const char *name : {"grade.hue", "grade.gamma", "grade.highlights", "grade.shadows"}) {
            if (!hasLoadedClip
                || !loaded.videoTracks.first().first().keyframes.track(QString::fromLatin1(name)))
                missing << QString::fromLatin1(name);
        }
        const bool keysSame = hasLoadedClip
            && sameKeyframes(clip.keyframes, loaded.videoTracks.first().first().keyframes);
        const QString second = hasLoadedClip
            ? ProjectFile::toJsonString(loaded)
            : QString();
        const bool byteIdentical = !second.isEmpty() && first.toUtf8() == second.toUtf8();
        check(11, "hue/gamma/highlights/shadows keyframes project roundtrip",
              missing.isEmpty() && keysSame && byteIdentical,
              QStringLiteral("projectLoaded=%1 missing=[%2] keysSame=%3 byteIdentical=%4 bytes=%5/%6")
                  .arg(projectLoaded ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(missing.join(QLatin1Char(',')))
                  .arg(keysSame ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(byteIdentical ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(first.toUtf8().size())
                  .arg(second.toUtf8().size()));
    }

    // G12: the graph editor's paste default for the four tracks is the
    // clip's static value; unknown grade names are rejected.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.colorCorrection.hue = 15.0;
        clip.colorCorrection.gamma = 1.3;
        clip.colorCorrection.highlights = 22.0;
        clip.colorCorrection.shadows = -9.0;
        struct Expect { const char *name; double value; };
        const Expect expects[] = {
            {"grade.hue", 15.0},
            {"grade.gamma", 1.3},
            {"grade.highlights", 22.0},
            {"grade.shadows", -9.0},
        };
        bool ok = true;
        QStringList details;
        for (const Expect &e : expects) {
            double v = -12345.0;
            const bool found = GraphEditorPanel::pasteTrackDefaultForTest(
                clip, QString::fromLatin1(e.name), v);
            ok = found && near(v, e.value) && ok;
            details << QStringLiteral("%1=%2(%3)")
                           .arg(QString::fromLatin1(e.name))
                           .arg(v, 0, 'g', 12)
                           .arg(found ? QStringLiteral("true") : QStringLiteral("false"));
        }
        double unknown = -12345.0;
        const bool unknownFound = GraphEditorPanel::pasteTrackDefaultForTest(
            clip, QStringLiteral("grade.nope"), unknown);
        ok = ok && !unknownFound;
        details << QStringLiteral("grade.nope=%1")
                       .arg(unknownFound ? QStringLiteral("true") : QStringLiteral("false"));
        check(12, "graph editor paste defaults for hue/gamma/highlights/shadows",
              ok, details.join(QLatin1Char(' ')));
    }

    return tally.finish("grade-keyframe");
}
