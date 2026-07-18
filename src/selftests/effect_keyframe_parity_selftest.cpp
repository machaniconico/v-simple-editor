// src/selftests/effect_keyframe_parity_selftest.cpp
// Render-time S2 effect-parameter keyframe parity gates.

#include "../clipanim/ClipAnim.h"
#include "../EffectControlsPanel.h"
#include "../EffectParamSchema.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstdio>

#include <QByteArray>
#include <QColor>
#include <QDebug>
#include <QMetaObject>
#include <QObject>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 64;
constexpr int kFps = 10;
constexpr int kFrameCount = 20;
constexpr double kEps = 1e-6;

QImage makePatternFrame()
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * 5 + y * 2) & 255;
            const int g = (x * 3 + y * 7) & 255;
            const int b = (x * 11 + y * 13 + 23) & 255;
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
    clip.displayName = QStringLiteral("effect_keyframe_parity");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

bool setTimelineClip(Timeline &timeline, const ClipInfo &clip, int trackIdx = 0)
{
    if (trackIdx < 0) {
        return false;
    }

    while (timeline.videoTracks().size() <= trackIdx) {
        timeline.addVideoTrack();
    }

    auto *track = timeline.videoTracks().value(trackIdx, nullptr);
    if (!track) {
        return false;
    }

    track->setClips(QVector<ClipInfo>{clip});
    return true;
}

bool invokeEffectPanelOperation(Timeline &timeline,
                                const char *slotName,
                                int effectIndex,
                                QString *detail,
                                int trackIdx = 0,
                                int clipIdx = 0)
{
    const auto &tracks = timeline.videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx]) {
        if (detail) {
            *detail = QStringLiteral("invalid target track=%1").arg(trackIdx);
        }
        return false;
    }
    if (clipIdx < 0 || clipIdx >= tracks[trackIdx]->clips().size()) {
        if (detail) {
            *detail = QStringLiteral("invalid target clip=%1").arg(clipIdx);
        }
        return false;
    }

    if (!tracks.isEmpty() && tracks.first() && !tracks.first()->clips().isEmpty()) {
        tracks.first()->setSelectedClip(0);
    }

    effectctrl::EffectControlsPanel panel;
    panel.setTimeline(&timeline);
    QObject::connect(&panel, &effectctrl::EffectControlsPanel::effectsChanged,
                     &timeline, [&timeline](const QVector<VideoEffect> &effects) {
        timeline.setClipEffects(effects);
    });
    const bool selected = QMetaObject::invokeMethod(&timeline,
                                                    "clipSelectedOnTrack",
                                                    Qt::DirectConnection,
                                                    Q_ARG(int, trackIdx),
                                                    Q_ARG(int, clipIdx));
    const bool invoked = QMetaObject::invokeMethod(&panel,
                                                   slotName,
                                                   Qt::DirectConnection,
                                                   Q_ARG(int, effectIndex));
    if ((!selected || !invoked) && detail) {
        *detail = QStringLiteral("selected=%1 invoked=%2")
                      .arg(selected ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(invoked ? QStringLiteral("true") : QStringLiteral("false"));
    }
    return selected && invoked;
}

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

bool sameEffect(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && near(a.param1, b.param1)
        && near(a.param2, b.param2)
        && near(a.param3, b.param3)
        && a.keyColor == b.keyColor;
}

bool sameEffects(const QVector<VideoEffect> &a, const QVector<VideoEffect> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (!sameEffect(a[i], b[i]))
            return false;
    }
    return true;
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
        if (!sameKeyframePoint(a.keyframes()[i], b.keyframes()[i])) {
            return false;
        }
    }
    return true;
}

bool sameStringKeyframeTrack(const StringKeyframeTrack &a, const StringKeyframeTrack &b)
{
    if (a.propertyName() != b.propertyName()
        || a.defaultValue() != b.defaultValue()
        || a.keyframes().size() != b.keyframes().size()) {
        return false;
    }
    for (int i = 0; i < a.keyframes().size(); ++i) {
        if (!near(a.keyframes()[i].time, b.keyframes()[i].time)
            || a.keyframes()[i].value != b.keyframes()[i].value) {
            return false;
        }
    }
    return true;
}

bool sameKeyframes(const KeyframeManager &a, const KeyframeManager &b)
{
    if (a.tracks().size() != b.tracks().size()
        || a.stringTracks().size() != b.stringTracks().size()) {
        return false;
    }
    for (const auto &track : a.tracks()) {
        const KeyframeTrack *other = b.track(track.propertyName());
        if (!other || !sameKeyframeTrack(track, *other)
            || a.loopOutMode(track.propertyName()) != b.loopOutMode(track.propertyName())) {
            return false;
        }
    }
    for (const auto &track : a.stringTracks()) {
        const StringKeyframeTrack *other = b.stringTrack(track.propertyName());
        if (!other || !sameStringKeyframeTrack(track, *other)) {
            return false;
        }
    }
    return true;
}

bool checkSingleUndoStep(const char *slotName,
                         int effectIndex,
                         const ClipInfo &clip,
                         QString *detail)
{
    Timeline timeline;
    setTimelineClip(timeline, clip);
    timeline.videoTracks().first()->setSelectedClip(0);
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("baseline"));
    const int beforeIndex = timeline.undoManager()->currentIndex();

    QString invokeDetail;
    const bool invoked = invokeEffectPanelOperation(timeline, slotName, effectIndex, &invokeDetail);
    const int afterIndex = timeline.undoManager()->currentIndex();
    const int increment = afterIndex - beforeIndex;
    bool ok = invoked && increment == 1;

    bool restored = false;
    if (timeline.canUndo()) {
        timeline.undo();
        const auto &clips = timeline.videoTracks().first()->clips();
        restored = clips.size() == 1
            && sameEffects(clips.first().effects, clip.effects)
            && sameKeyframes(clips.first().keyframes, clip.keyframes);
        ok = ok && restored;
    }

    if (!ok && detail) {
        *detail = QStringLiteral("%1 invoked=%2 before=%3 after=%4 increment=%5 restored=%6 %7")
                      .arg(QString::fromLatin1(slotName))
                      .arg(invoked ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(beforeIndex)
                      .arg(afterIndex)
                      .arg(increment)
                      .arg(restored ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(invokeDetail);
    }
    return ok;
}

} // namespace

int runEffectKeyframeParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[effect-keyframe-parity] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    QString clipPath;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }
    clipPath = tempDir.filePath(QStringLiteral("effect_keyframe_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: no effect.* keyframes returns the static effect stack unchanged.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(7.0));
        clip.effects.append(VideoEffect::createVignette(0.35, 0.72));

        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);

        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, 0.5);
        check(1, "no effect keyframes == static effect values",
              sameEffects(effects, clip.effects));
    }

    // G2: renderFrameAt without effect.* keyframes matches a static FX oracle.
    {
        const QSize outSize(80, 60);
        const double sourceSec = 0.5;
        const qint64 usec = 500000;
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(6.0));

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QImage rendered = tlrender::renderFrameAt(&timeline, usec, outSize);

        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sourceSec);
        QImage expected;
        QString detail;
        bool ok = !native.isNull() && !rendered.isNull();
        if (ok) {
            expected = VideoEffectProcessor::applyEffectStack(
                           native, ColorCorrection(), clip.effects)
                           .convertToFormat(QImage::Format_RGBA8888)
                           .scaled(outSize, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
        } else if (native.isNull()) {
            detail = QStringLiteral("decodeClipFrameNativeForTest returned null");
        } else {
            detail = QStringLiteral("renderFrameAt returned null");
        }

        const double mse = ok ? framediff::mse(rendered, expected) : -1.0;
        check(2, "keyframe-free renderFrameAt == static FX oracle",
              ok && mse == 0.0,
              ok ? QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12) : detail);
    }

    // G3: effect.0.radius interpolation is delegated to KeyframeManager::valueAt.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 1.0);
        radius.addKeyframe(1.0, 11.0);
        clip.keyframes.addTrack(radius);

        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, 0.5);
        const double value = effects.isEmpty()
            ? -1.0
            : effectctrl::paramValue(effects.first(), QStringLiteral("radius"));
        check(3, "Blur radius linear interpolation at t=0.5",
              near(value, 6.0, 1e-5),
              QStringLiteral("radius=%1").arg(value, 0, 'g', 12));
    }

    // G4: renderFrameAt changes over clip-local time when an effect param is keyed.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.speed = 0.000001;
        clip.effects.append(VideoEffect::createBlur(1.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 1.0);
        radius.addKeyframe(1.0, 18.0);
        clip.keyframes.addTrack(radius);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(80, 60);
        const QImage a = tlrender::renderFrameAt(&timeline, 0, outSize);
        const QImage b = tlrender::renderFrameAt(&timeline, 1'000'000, outSize);
        const double mse = framediff::mse(a, b);
        check(4, "renderFrameAt output changes with keyed Blur radius",
              mse > 0.0,
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    // G5: moving effects swaps their effect.<index>.* keyframe track groups.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        clip.effects.append(VideoEffect::createVignette(0.2, 0.4));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 2.0);
        radius.addKeyframe(1.0, 12.0);
        clip.keyframes.addTrack(radius);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        QString detail;
        bool ok = invokeEffectPanelOperation(timeline, "onMoveDownRequested", 0, &detail);
        const ClipInfo &edited = timeline.videoTracks().first()->clips().first();
        const QVector<VideoEffect> effects = clipanim::effectiveEffectsAt(edited, 0.5);
        const double movedRadius = effects.size() > 1
            ? effectctrl::paramValue(effects[1], QStringLiteral("radius"))
            : -1.0;
        ok = ok
            && effects.size() == 2
            && effects[0].type == VideoEffectType::Vignette
            && effects[1].type == VideoEffectType::Blur
            && !edited.keyframes.hasTrack(QStringLiteral("effect.0.radius"))
            && edited.keyframes.hasTrack(QStringLiteral("effect.1.radius"))
            && near(movedRadius, 7.0, 1e-5);
        check(5, "effect reorder keeps keyframes bound to the moved effect",
              ok,
              ok ? QString() : QStringLiteral("radius=%1 %2").arg(movedRadius, 0, 'g', 12).arg(detail));
    }

    // G6: removing an effect deletes its keyframes and shifts later effect tracks down.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        clip.effects.append(VideoEffect::createSharpen(1.0));
        clip.effects.append(VideoEffect::createNoise(5.0));
        KeyframeTrack removedAmount(QStringLiteral("effect.1.amount"), 1.0);
        removedAmount.addKeyframe(0.0, 8.0);
        removedAmount.addKeyframe(1.0, 9.0);
        clip.keyframes.addTrack(removedAmount);
        KeyframeTrack shiftedAmount(QStringLiteral("effect.2.amount"), 5.0);
        shiftedAmount.addKeyframe(0.0, 30.0);
        shiftedAmount.addKeyframe(1.0, 50.0);
        clip.keyframes.addTrack(shiftedAmount);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        QString detail;
        bool ok = invokeEffectPanelOperation(timeline, "onRemoveRequested", 1, &detail);
        const ClipInfo &edited = timeline.videoTracks().first()->clips().first();
        const QVector<VideoEffect> effects = clipanim::effectiveEffectsAt(edited, 0.5);
        const double shiftedValue = effects.size() > 1
            ? effectctrl::paramValue(effects[1], QStringLiteral("amount"))
            : -1.0;
        ok = ok
            && effects.size() == 2
            && effects[1].type == VideoEffectType::Noise
            && edited.keyframes.hasTrack(QStringLiteral("effect.1.amount"))
            && !edited.keyframes.hasTrack(QStringLiteral("effect.2.amount"))
            && near(shiftedValue, 40.0, 1e-5);
        check(6, "effect remove drops removed keyframes and shifts later tracks down",
              ok,
              ok ? QString() : QStringLiteral("amount=%1 %2").arg(shiftedValue, 0, 'g', 12).arg(detail));
    }

    // G7: duplicating an effect copies its keyframes and shifts later effect tracks up.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        clip.effects.append(VideoEffect::createNoise(5.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 2.0);
        radius.addKeyframe(1.0, 12.0);
        clip.keyframes.addTrack(radius);
        KeyframeTrack amount(QStringLiteral("effect.1.amount"), 5.0);
        amount.addKeyframe(0.0, 30.0);
        amount.addKeyframe(1.0, 50.0);
        clip.keyframes.addTrack(amount);

        Timeline timeline;
        setTimelineClip(timeline, clip);
        QString detail;
        bool ok = invokeEffectPanelOperation(timeline, "onDuplicateRequested", 0, &detail);
        const ClipInfo &edited = timeline.videoTracks().first()->clips().first();
        const QVector<VideoEffect> effects = clipanim::effectiveEffectsAt(edited, 0.5);
        const double sourceRadius = !effects.isEmpty()
            ? effectctrl::paramValue(effects[0], QStringLiteral("radius"))
            : -1.0;
        const double copiedRadius = effects.size() > 1
            ? effectctrl::paramValue(effects[1], QStringLiteral("radius"))
            : -1.0;
        const double shiftedAmount = effects.size() > 2
            ? effectctrl::paramValue(effects[2], QStringLiteral("amount"))
            : -1.0;
        ok = ok
            && effects.size() == 3
            && effects[0].type == VideoEffectType::Blur
            && effects[1].type == VideoEffectType::Blur
            && effects[2].type == VideoEffectType::Noise
            && edited.keyframes.hasTrack(QStringLiteral("effect.0.radius"))
            && edited.keyframes.hasTrack(QStringLiteral("effect.1.radius"))
            && !edited.keyframes.hasTrack(QStringLiteral("effect.1.amount"))
            && edited.keyframes.hasTrack(QStringLiteral("effect.2.amount"))
            && near(sourceRadius, 7.0, 1e-5)
            && near(copiedRadius, 7.0, 1e-5)
            && near(shiftedAmount, 40.0, 1e-5);
        check(7, "effect duplicate copies source keyframes and shifts later tracks up",
              ok,
              ok ? QString()
                 : QStringLiteral("srcRadius=%1 copyRadius=%2 amount=%3 %4")
                       .arg(sourceRadius, 0, 'g', 12)
                       .arg(copiedRadius, 0, 'g', 12)
                       .arg(shiftedAmount, 0, 'g', 12)
                       .arg(detail));
    }

    // G8: the structural effect operations save exactly one undo entry each.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createBlur(1.0));
        clip.effects.append(VideoEffect::createSharpen(1.0));
        clip.effects.append(VideoEffect::createNoise(5.0));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 2.0);
        radius.addKeyframe(1.0, 12.0);
        clip.keyframes.addTrack(radius);
        KeyframeTrack amount(QStringLiteral("effect.1.amount"), 1.0);
        amount.addKeyframe(0.0, 8.0);
        amount.addKeyframe(1.0, 9.0);
        clip.keyframes.addTrack(amount);
        KeyframeTrack shiftedAmount(QStringLiteral("effect.2.amount"), 5.0);
        shiftedAmount.addKeyframe(0.0, 30.0);
        shiftedAmount.addKeyframe(1.0, 50.0);
        clip.keyframes.addTrack(shiftedAmount);

        QString detail;
        bool ok = true;
        ok = checkSingleUndoStep("onDuplicateRequested", 0, clip, &detail) && ok;
        ok = checkSingleUndoStep("onRemoveRequested", 1, clip, &detail) && ok;
        ok = checkSingleUndoStep("onMoveUpRequested", 1, clip, &detail) && ok;
        ok = checkSingleUndoStep("onMoveDownRequested", 1, clip, &detail) && ok;
        check(8, "effect operations add one undo state and undo restores the prior stack",
              ok,
              ok ? QString() : detail);
    }

    // G9: V2 effect operations update the V2 clip, even if V1 has a selection.
    {
        ClipInfo v1Clip = makeClip(clipPath);
        v1Clip.displayName = QStringLiteral("v1_guard");
        v1Clip.effects.append(VideoEffect::createSharpen(3.0));
        KeyframeTrack v1Amount(QStringLiteral("effect.0.amount"), 3.0);
        v1Amount.addKeyframe(0.0, 3.0);
        v1Amount.addKeyframe(1.0, 4.0);
        v1Clip.keyframes.addTrack(v1Amount);

        ClipInfo v2Clip = makeClip(clipPath);
        v2Clip.displayName = QStringLiteral("v2_target");
        v2Clip.effects.append(VideoEffect::createBlur(1.0));
        v2Clip.effects.append(VideoEffect::createVignette(0.2, 0.4));
        KeyframeTrack radius(QStringLiteral("effect.0.radius"), 1.0);
        radius.addKeyframe(0.0, 2.0);
        radius.addKeyframe(1.0, 12.0);
        v2Clip.keyframes.addTrack(radius);

        Timeline timeline;
        bool ok = setTimelineClip(timeline, v1Clip, 0)
            && setTimelineClip(timeline, v2Clip, 1);
        QString detail;
        ok = invokeEffectPanelOperation(timeline, "onMoveDownRequested", 0,
                                        &detail, 1, 0) && ok;

        const auto &tracks = timeline.videoTracks();
        const bool tracksReady = tracks.size() > 1
            && tracks[0]
            && tracks[1]
            && !tracks[0]->clips().isEmpty()
            && !tracks[1]->clips().isEmpty();
        const ClipInfo *editedV1 = tracksReady ? &tracks[0]->clips().first() : nullptr;
        const ClipInfo *editedV2 = tracksReady ? &tracks[1]->clips().first() : nullptr;
        const QVector<VideoEffect> v2Effects = editedV2
            ? clipanim::effectiveEffectsAt(*editedV2, 0.5)
            : QVector<VideoEffect>{};
        const double movedRadius = v2Effects.size() > 1
            ? effectctrl::paramValue(v2Effects[1], QStringLiteral("radius"))
            : -1.0;
        const bool v1EffectsSame = editedV1 && sameEffects(editedV1->effects, v1Clip.effects);
        const bool v1KeyframesSame = editedV1 && sameKeyframes(editedV1->keyframes, v1Clip.keyframes);

        ok = ok
            && tracksReady
            && v1EffectsSame
            && v1KeyframesSame
            && v2Effects.size() == 2
            && v2Effects[0].type == VideoEffectType::Vignette
            && v2Effects[1].type == VideoEffectType::Blur
            && editedV2
            && !editedV2->keyframes.hasTrack(QStringLiteral("effect.0.radius"))
            && editedV2->keyframes.hasTrack(QStringLiteral("effect.1.radius"))
            && near(movedRadius, 7.0, 1e-5);
        check(9, "V2 effect reorder writes V2 and leaves selected V1 unchanged",
              ok,
              ok ? QString()
                 : QStringLiteral("radius=%1 tracksReady=%2 v1EffectsSame=%3 v1KeyframesSame=%4 %5")
                       .arg(movedRadius, 0, 'g', 12)
                       .arg(tracksReady ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(v1EffectsSame ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(v1KeyframesSame ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(detail));
    }

    qInfo().noquote()
        << QStringLiteral("EFFECT-KEYFRAME-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
