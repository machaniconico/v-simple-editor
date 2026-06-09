#include "../MotionPreset.h"
#include "../Timeline.h"
#include "../clipanim/ClipAnim.h"

#include <cmath>

#include <QDebug>
#include <QSet>
#include <QStringList>

namespace {

const QString kScaleTrack = QStringLiteral("motion.scale");
const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");
const QString kRotationTrack = QStringLiteral("motion.rotation");
const QString kOpacityTrack = QStringLiteral("motion.opacity");

bool near(double a, double b, double eps = 1e-5)
{
    return std::abs(a - b) <= eps;
}

bool hasMotionKeyframes(const KeyframeManager &km)
{
    const QStringList tracks = {
        kScaleTrack, kPosXTrack, kPosYTrack, kRotationTrack, kOpacityTrack
    };
    for (const QString &name : tracks) {
        const KeyframeTrack *track = km.track(name);
        if (track && track->count() > 0)
            return true;
    }
    return false;
}

int motionKeyframeCount(const KeyframeManager &km)
{
    int count = 0;
    const QStringList tracks = {
        kScaleTrack, kPosXTrack, kPosYTrack, kRotationTrack, kOpacityTrack
    };
    for (const QString &name : tracks) {
        const KeyframeTrack *track = km.track(name);
        if (track)
            count += track->count();
    }
    return count;
}

ClipInfo makeClip(const KeyframeManager &km = KeyframeManager())
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("motion_preset_selftest");
    clip.displayName = QStringLiteral("motion_preset_selftest");
    clip.duration = 3.0;
    clip.outPoint = clip.duration;
    clip.videoScale = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    clip.rotation2DDegrees = 0.0;
    clip.opacity = 1.0;
    clip.keyframes = km;
    return clip;
}

} // namespace

int runMotionPresetSelftest()
{
    qInfo().noquote() << "[motion-preset] selftest start";
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const QString &label, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[motion-preset] G%1 PASS: %2")
                                     .arg(gate).arg(label);
        } else {
            ++failed;
            qCritical().noquote()
                << QStringLiteral("[motion-preset] G%1 FAIL: %2%3")
                       .arg(gate)
                       .arg(label)
                       .arg(detail.isEmpty() ? QString()
                                             : QStringLiteral(" -- ") + detail);
        }
    };

    // G1: preset list shape, unique ids, and non-empty Japanese labels.
    {
        const QStringList ids = motionpreset::presetIds();
        QSet<QString> unique;
        bool ok = ids.size() >= 8 && ids.size() <= 10;
        for (const QString &id : ids) {
            ok = ok && !id.isEmpty();
            ok = ok && !motionpreset::displayName(id).isEmpty();
            unique.insert(id);
        }
        ok = ok && unique.size() == ids.size();
        check(1, QStringLiteral("preset ids unique with display names"), ok,
              QStringLiteral("count=%1 unique=%2").arg(ids.size()).arg(unique.size()));
    }

    // G2: FadeIn writes opacity from 0 to 1 over the lead.
    {
        KeyframeManager km;
        motionpreset::applyPreset(km, QStringLiteral("FadeIn"), 0.0, 3.0);
        ClipInfo clip = makeClip(km);
        const KeyframeTrack *opacity = km.track(kOpacityTrack);
        bool ok = opacity && opacity->count() >= 2;
        double startTime = 0.0;
        double endTime = 0.0;
        if (ok) {
            startTime = opacity->keyframes().first().time;
            endTime = opacity->keyframes().last().time;
            ok = near(startTime, 0.0)
                && near(clipanim::effectiveOpacityAt(clip, startTime, clip.opacity), 0.0)
                && near(clipanim::effectiveOpacityAt(clip, endTime, clip.opacity), 1.0);
        }
        check(2, QStringLiteral("FadeIn opacity 0 -> 1"), ok,
              QStringLiteral("start=%1 end=%2").arg(startTime).arg(endTime));
    }

    // G3: SlideInLeft moves x from off-screen-left to identity.
    {
        KeyframeManager km;
        motionpreset::applyPreset(km, QStringLiteral("SlideInLeft"), 0.0, 3.0);
        ClipInfo clip = makeClip(km);
        const KeyframeTrack *posX = km.track(kPosXTrack);
        bool ok = posX && posX->count() >= 2;
        double x0 = 0.0;
        double x1 = 0.0;
        if (ok) {
            const double startTime = posX->keyframes().first().time;
            const double endTime = posX->keyframes().last().time;
            x0 = clipanim::effectiveTransformAt(clip, startTime).videoDx;
            x1 = clipanim::effectiveTransformAt(clip, endTime).videoDx;
            ok = x0 < 0.0 && !near(x0, 0.0) && near(x1, 0.0);
        }
        check(3, QStringLiteral("SlideInLeft x settles at identity"), ok,
              QStringLiteral("x0=%1 x1=%2").arg(x0).arg(x1));
    }

    // G4: PopIn and BounceIn start below 1.0 scale and settle at 1.0.
    {
        bool ok = true;
        QString detail;
        const QStringList ids = { QStringLiteral("PopIn"), QStringLiteral("BounceIn") };
        for (const QString &id : ids) {
            KeyframeManager km;
            motionpreset::applyPreset(km, id, 0.0, 3.0);
            ClipInfo clip = makeClip(km);
            const KeyframeTrack *scale = km.track(kScaleTrack);
            if (!scale || scale->count() < 2) {
                ok = false;
                detail = id + QStringLiteral(": missing scale track");
                break;
            }
            const double startTime = scale->keyframes().first().time;
            const double endTime = scale->keyframes().last().time;
            const double s0 = clipanim::effectiveTransformAt(clip, startTime).videoScale;
            const double s1 = clipanim::effectiveTransformAt(clip, endTime).videoScale;
            if (!(s0 < 1.0 && near(s1, 1.0))) {
                ok = false;
                detail = QStringLiteral("%1: s0=%2 s1=%3").arg(id).arg(s0).arg(s1);
                break;
            }
        }
        check(4, QStringLiteral("PopIn/BounceIn scale starts small and settles"), ok, detail);
    }

    // G5: applying a preset replaces canonical motion tracks, so repeated
    // application is deterministic and does not append stale motion keys.
    {
        KeyframeManager km;
        KeyframeTrack manualRotation(kRotationTrack, 0.0);
        manualRotation.addKeyframe(0.0, 33.0);
        manualRotation.addKeyframe(1.0, 44.0);
        km.addTrack(manualRotation);

        motionpreset::applyPreset(km, QStringLiteral("FadeIn"), 0.0, 3.0);
        const int firstCount = motionKeyframeCount(km);
        const bool replacedManual = !km.hasTrack(kRotationTrack);
        motionpreset::applyPreset(km, QStringLiteral("FadeIn"), 0.0, 3.0);
        const int secondCount = motionKeyframeCount(km);

        check(5, QStringLiteral("preset application replaces, not appends, motion tracks"),
              replacedManual && firstCount == secondCount && firstCount == 2,
              QStringLiteral("first=%1 second=%2 rotationTrack=%3")
                  .arg(firstCount)
                  .arg(secondCount)
                  .arg(km.hasTrack(kRotationTrack) ? 1 : 0));
    }

    // G6: a clip with no preset has no motion keyframes and clipanim returns
    // the static identity values.
    {
        ClipInfo clip = makeClip();
        clip.videoScale = 1.35;
        clip.videoDx = 0.2;
        clip.videoDy = -0.1;
        clip.rotation2DDegrees = 12.0;
        clip.opacity = 0.42;
        const clipgeom::ClipTransform transform =
            clipanim::effectiveTransformAt(clip, 0.5);
        const double opacity = clipanim::effectiveOpacityAt(clip, 0.5, clip.opacity);
        const bool ok = !hasMotionKeyframes(clip.keyframes)
            && near(transform.videoScale, clip.videoScale)
            && near(transform.videoDx, clip.videoDx)
            && near(transform.videoDy, clip.videoDy)
            && near(transform.rotationDeg, clip.rotation2DDegrees)
            && near(opacity, clip.opacity);
        check(6, QStringLiteral("no preset leaves clipanim static identity path"), ok);
    }

    qInfo().noquote()
        << QStringLiteral("[motion-preset] selftest end, passed=%1 failed=%2")
               .arg(passed).arg(failed);
    return failed == 0 ? 0 : 1;
}
