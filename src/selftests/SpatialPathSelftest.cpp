// src/selftests/SpatialPathSelftest.cpp
// AE-ANIM-2: 2D spatial Bezier position path regression gates.

#include "../clipanim/ClipAnim.h"
#include "../Keyframe.h"
#include "../Timeline.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>

namespace {

const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");

bool exactly(double a, double b)
{
    return a == b;
}

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

bool sameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

bool isFiniteValue(double value)
{
    return std::isfinite(value);
}

ClipInfo makeClip()
{
    ClipInfo clip;
    clip.duration = 1.0;
    clip.inPoint = 0.0;
    clip.outPoint = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    return clip;
}

ClipInfo makeLoopedSpatialClip(LoopMode mode)
{
    ClipInfo clip = makeClip();
    KeyframeTrack x(kPosXTrack, 0.0);
    x.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                  0.0, 0.0, 1.0, 1.0,
                  true, 0.25, 0.75, 0.0, 0.0);
    x.addKeyframe(1.0, 1.0, KeyframePoint::Linear,
                  0.0, 0.0, 1.0, 1.0,
                  true, 0.0, 0.0, -0.25, 0.75);
    KeyframeTrack y(kPosYTrack, 0.0);
    y.addKeyframe(0.0, 0.0);
    y.addKeyframe(1.0, 0.0);
    clip.keyframes.addTrack(x);
    clip.keyframes.addTrack(y);
    clip.keyframes.setLoopOutMode(kPosXTrack, mode);
    clip.keyframes.setLoopOutMode(kPosYTrack, mode);
    return clip;
}

QJsonObject firstKeyframeObject(const KeyframeManager &manager)
{
    const QJsonObject root = manager.toJson();
    const QJsonArray tracks = root[QStringLiteral("tracks")].toArray();
    if (tracks.isEmpty())
        return {};
    const QJsonArray keyframes =
        tracks.at(0).toObject()[QStringLiteral("keyframes")].toArray();
    if (keyframes.isEmpty())
        return {};
    return keyframes.at(0).toObject();
}

bool hasAnySpatialKey(const QJsonObject &obj)
{
    return obj.contains(QStringLiteral("spatialOutX"))
        || obj.contains(QStringLiteral("spatialOutY"))
        || obj.contains(QStringLiteral("spatialInX"))
        || obj.contains(QStringLiteral("spatialInY"));
}

} // namespace

int runSpatialPathSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[spatial-path] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    // G1: no spatial tangents uses exactly the independent per-axis result.
    {
        ClipInfo clip = makeClip();

        KeyframeTrack x(kPosXTrack, 0.0);
        x.addKeyframe(0.0, -0.25, KeyframePoint::EaseInOut);
        x.addKeyframe(1.0, 0.75, KeyframePoint::Linear);

        KeyframeTrack y(kPosYTrack, 0.0);
        y.addKeyframe(0.0, 0.20, KeyframePoint::Bezier, 0.42, 0.0, 1.0, 1.0);
        y.addKeyframe(1.0, -0.30, KeyframePoint::Linear);

        clip.keyframes.addTrack(x);
        clip.keyframes.addTrack(y);

        bool ok = true;
        QString detail;
        const double samples[] = {0.0, 0.125, 0.25, 0.5, 0.75, 1.0};
        for (double t : samples) {
            const double expectedX = clip.keyframes.valueAt(kPosXTrack, t, clip.videoDx);
            const double expectedY = clip.keyframes.valueAt(kPosYTrack, t, clip.videoDy);
            const clipgeom::ClipTransform xf = clipanim::effectiveTransformAt(clip, t);
            const QPointF pos = clipanim::effectivePositionAt(clip, t);
            if (!exactly(xf.videoDx, expectedX)
                || !exactly(xf.videoDy, expectedY)
                || !exactly(pos.x(), expectedX)
                || !exactly(pos.y(), expectedY)) {
                ok = false;
                detail = QStringLiteral("t=%1 expected=(%2,%3) transform=(%4,%5) pos=(%6,%7)")
                    .arg(t, 0, 'g', 12)
                    .arg(expectedX, 0, 'g', 17)
                    .arg(expectedY, 0, 'g', 17)
                    .arg(xf.videoDx, 0, 'g', 17)
                    .arg(xf.videoDy, 0, 'g', 17)
                    .arg(pos.x(), 0, 'g', 17)
                    .arg(pos.y(), 0, 'g', 17);
                break;
            }
        }
        check(1, "no tangents equals per-axis interpolation exactly", ok, detail);
    }

    // G2: spatial tangents bow the midpoint away from the straight line.
    {
        ClipInfo clip = makeClip();
        KeyframeTrack x(kPosXTrack, 0.0);
        x.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                      0.0, 0.0, 1.0, 1.0,
                      true, 0.25, 0.75, 0.0, 0.0);
        x.addKeyframe(1.0, 1.0, KeyframePoint::Linear,
                      0.0, 0.0, 1.0, 1.0,
                      true, 0.0, 0.0, -0.25, 0.75);
        KeyframeTrack y(kPosYTrack, 0.0);
        y.addKeyframe(0.0, 0.0);
        y.addKeyframe(1.0, 0.0);
        clip.keyframes.addTrack(x);
        clip.keyframes.addTrack(y);

        const QPointF pos = clipanim::effectivePositionAt(clip, 0.5);
        check(2, "Bezier midpoint bows off straight line",
              isFiniteValue(pos.y()) && std::abs(pos.y()) > 0.1,
              QStringLiteral("mid=(%1,%2)")
                  .arg(pos.x(), 0, 'g', 12)
                  .arg(pos.y(), 0, 'g', 12));
    }

    // G3: endpoints are exact regardless of tangent handles.
    {
        ClipInfo clip = makeClip();
        KeyframeTrack x(kPosXTrack, 0.0);
        x.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                      0.0, 0.0, 1.0, 1.0,
                      true, 0.25, 0.75, 0.0, 0.0);
        x.addKeyframe(1.0, 1.0, KeyframePoint::Linear,
                      0.0, 0.0, 1.0, 1.0,
                      true, 0.0, 0.0, -0.25, 0.75);
        KeyframeTrack y(kPosYTrack, 0.0);
        y.addKeyframe(0.0, 0.0);
        y.addKeyframe(1.0, 0.0);
        clip.keyframes.addTrack(x);
        clip.keyframes.addTrack(y);

        const QPointF atStart = clipanim::effectivePositionAt(clip, 0.0);
        const QPointF atEnd = clipanim::effectivePositionAt(clip, 1.0);
        check(3, "spatial path endpoints exact",
              exactly(atStart.x(), 0.0)
                  && exactly(atStart.y(), 0.0)
                  && exactly(atEnd.x(), 1.0)
                  && exactly(atEnd.y(), 0.0),
              QStringLiteral("start=(%1,%2) end=(%3,%4)")
                  .arg(atStart.x(), 0, 'g', 17)
                  .arg(atStart.y(), 0, 'g', 17)
                  .arg(atEnd.x(), 0, 'g', 17)
                  .arg(atEnd.y(), 0, 'g', 17));
    }

    // G4: spatial tangents round-trip through JSON, absent tangents are omitted.
    {
        KeyframeManager spatialManager;
        KeyframeTrack spatialTrack(kPosXTrack, 0.0);
        spatialTrack.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                                 0.0, 0.0, 1.0, 1.0,
                                 true, 0.25, 0.75, -0.10, 0.20);
        spatialTrack.addKeyframe(1.0, 1.0);
        spatialManager.addTrack(spatialTrack);

        const QJsonObject spatialJson = spatialManager.toJson();
        const QJsonObject spatialKf = firstKeyframeObject(spatialManager);
        KeyframeManager loaded;
        loaded.fromJson(spatialJson);
        const KeyframeTrack *loadedTrack = loaded.track(kPosXTrack);
        bool ok = loadedTrack && loadedTrack->count() == 2;
        if (ok) {
            const KeyframePoint &kf = loadedTrack->keyframes().first();
            ok = kf.hasSpatialTangent
                && near(kf.spatialOutX, 0.25)
                && near(kf.spatialOutY, 0.75)
                && near(kf.spatialInX, -0.10)
                && near(kf.spatialInY, 0.20)
                && spatialKf.contains(QStringLiteral("spatialOutX"))
                && spatialKf.contains(QStringLiteral("spatialOutY"))
                && spatialKf.contains(QStringLiteral("spatialInX"))
                && spatialKf.contains(QStringLiteral("spatialInY"));
        }

        KeyframeManager legacyManager;
        KeyframeTrack legacyTrack(kPosXTrack, 0.0);
        legacyTrack.addKeyframe(0.0, 0.0);
        legacyTrack.addKeyframe(1.0, 1.0);
        legacyManager.addTrack(legacyTrack);
        ok = ok && !hasAnySpatialKey(firstKeyframeObject(legacyManager));

        check(4, "spatial JSON round-trip and absent JSON omission", ok);
    }

    // G5: one-sided and invalid/degenerate tangents do not produce NaN/crash.
    {
        ClipInfo oneSided = makeClip();
        KeyframeTrack x(kPosXTrack, 0.0);
        x.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                      0.0, 0.0, 1.0, 1.0,
                      true, 0.25, 0.50, 0.0, 0.0);
        x.addKeyframe(1.0, 1.0);
        KeyframeTrack y(kPosYTrack, 0.0);
        y.addKeyframe(0.0, 0.0);
        y.addKeyframe(1.0, 0.0);
        oneSided.keyframes.addTrack(x);
        oneSided.keyframes.addTrack(y);
        const QPointF oneSidedMid = clipanim::effectivePositionAt(oneSided, 0.5);

        ClipInfo degenerate = makeClip();
        KeyframeTrack degenerateX(kPosXTrack, 0.0);
        degenerateX.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                                0.0, 0.0, 1.0, 1.0,
                                true, 0.0, 0.0, 0.0, 0.0);
        degenerateX.addKeyframe(1.0, 1.0, KeyframePoint::Linear,
                                0.0, 0.0, 1.0, 1.0,
                                true, 0.0, 0.0, 0.0, 0.0);
        KeyframeTrack degenerateY(kPosYTrack, 0.0);
        degenerateY.addKeyframe(0.0, 0.0);
        degenerateY.addKeyframe(1.0, 0.0);
        degenerate.keyframes.addTrack(degenerateX);
        degenerate.keyframes.addTrack(degenerateY);
        const QPointF degenerateMid = clipanim::effectivePositionAt(degenerate, 0.5);

        ClipInfo invalid = makeClip();
        KeyframeTrack invalidX(kPosXTrack, 0.0);
        invalidX.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                             0.0, 0.0, 1.0, 1.0,
                             true, std::numeric_limits<double>::quiet_NaN(),
                             0.0, 0.0, 0.0);
        invalidX.addKeyframe(1.0, 1.0);
        KeyframeTrack invalidY(kPosYTrack, 0.0);
        invalidY.addKeyframe(0.0, 0.0);
        invalidY.addKeyframe(1.0, 0.0);
        invalid.keyframes.addTrack(invalidX);
        invalid.keyframes.addTrack(invalidY);
        const QPointF invalidMid = clipanim::effectivePositionAt(invalid, 0.5);
        const double expectedInvalidX = invalid.keyframes.valueAt(kPosXTrack, 0.5, invalid.videoDx);
        const double expectedInvalidY = invalid.keyframes.valueAt(kPosYTrack, 0.5, invalid.videoDy);

        check(5, "one-sided/degenerate/invalid tangents are finite and sensible",
              isFiniteValue(oneSidedMid.x())
                  && isFiniteValue(oneSidedMid.y())
                  && isFiniteValue(degenerateMid.x())
                  && isFiniteValue(degenerateMid.y())
                  && exactly(degenerateMid.x(), 0.5)
                  && exactly(degenerateMid.y(), 0.0)
                  && isFiniteValue(invalidMid.x())
                  && isFiniteValue(invalidMid.y())
                  && exactly(invalidMid.x(), expectedInvalidX)
                  && exactly(invalidMid.y(), expectedInvalidY),
              QStringLiteral("one-sided=(%1,%2) degenerate=(%3,%4) invalid=(%5,%6)")
                  .arg(oneSidedMid.x(), 0, 'g', 12)
                  .arg(oneSidedMid.y(), 0, 'g', 12)
                  .arg(degenerateMid.x(), 0, 'g', 12)
                  .arg(degenerateMid.y(), 0, 'g', 12)
                  .arg(invalidMid.x(), 0, 'g', 12)
                  .arg(invalidMid.y(), 0, 'g', 12));
    }

    // G6: loopOut Cycle keeps spatial path evaluation on the curved path.
    {
        ClipInfo clip = makeLoopedSpatialClip(LoopMode::Cycle);
        const QPointF firstCycle = clipanim::effectivePositionAt(clip, 0.5);
        const QPointF secondCycle = clipanim::effectivePositionAt(clip, 1.5);
        const clipgeom::ClipTransform secondTransform =
            clipanim::effectiveTransformAt(clip, 1.5);
        check(6, "Cycle loop keeps Bezier spatial path on second cycle",
              isFiniteValue(secondCycle.x())
                  && isFiniteValue(secondCycle.y())
                  && near(secondCycle.x(), firstCycle.x())
                  && near(secondCycle.y(), firstCycle.y())
                  && near(secondTransform.videoDx, firstCycle.x())
                  && near(secondTransform.videoDy, firstCycle.y())
                  && std::abs(secondCycle.y()) > 0.1,
              QStringLiteral("first=(%1,%2) second=(%3,%4) transform=(%5,%6)")
                  .arg(firstCycle.x(), 0, 'g', 12)
                  .arg(firstCycle.y(), 0, 'g', 12)
                  .arg(secondCycle.x(), 0, 'g', 12)
                  .arg(secondCycle.y(), 0, 'g', 12)
                  .arg(secondTransform.videoDx, 0, 'g', 12)
                  .arg(secondTransform.videoDy, 0, 'g', 12));
    }

    // G7: loopOut PingPong folds reversed phases before spatial segment lookup.
    {
        ClipInfo clip = makeLoopedSpatialClip(LoopMode::PingPong);
        const QPointF inRange = clipanim::effectivePositionAt(clip, 0.75);
        const QPointF pingPong = clipanim::effectivePositionAt(clip, 1.25);
        const clipgeom::ClipTransform pingPongTransform =
            clipanim::effectiveTransformAt(clip, 1.25);
        check(7, "PingPong loop keeps Bezier spatial path on reversed cycle",
              isFiniteValue(pingPong.x())
                  && isFiniteValue(pingPong.y())
                  && near(pingPong.x(), inRange.x())
                  && near(pingPong.y(), inRange.y())
                  && near(pingPongTransform.videoDx, inRange.x())
                  && near(pingPongTransform.videoDy, inRange.y())
                  && std::abs(pingPong.y()) > 0.1,
              QStringLiteral("in-range=(%1,%2) pingpong=(%3,%4) transform=(%5,%6)")
                  .arg(inRange.x(), 0, 'g', 12)
                  .arg(inRange.y(), 0, 'g', 12)
                  .arg(pingPong.x(), 0, 'g', 12)
                  .arg(pingPong.y(), 0, 'g', 12)
                  .arg(pingPongTransform.videoDx, 0, 'g', 12)
                  .arg(pingPongTransform.videoDy, 0, 'g', 12));
    }

    // G8: auto-orient follows the path and preserves static rotation as offset.
    {
        auto makeDirectionalClip = [](const QPointF& end, double rotation) {
            ClipInfo clip = makeClip();
            clip.autoOrientEnabled = true;
            clip.rotation2DDegrees = rotation;
            KeyframeTrack x(kPosXTrack, 0.0);
            x.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                          0.0, 0.0, 1.0, 1.0,
                          true, end.x() * 0.25, end.y() * 0.25, 0.0, 0.0);
            x.addKeyframe(1.0, end.x(), KeyframePoint::Linear,
                          0.0, 0.0, 1.0, 1.0,
                          true, 0.0, 0.0, -end.x() * 0.25, -end.y() * 0.25);
            KeyframeTrack y(kPosYTrack, 0.0);
            y.addKeyframe(0.0, 0.0);
            y.addKeyframe(1.0, end.y());
            clip.keyframes.addTrack(x);
            clip.keyframes.addTrack(y);
            return clip;
        };

        const ClipInfo right = makeDirectionalClip(QPointF(1.0, 0.0), 0.0);
        const ClipInfo down = makeDirectionalClip(QPointF(0.0, 1.0), 0.0);
        const ClipInfo downWithOffset =
            makeDirectionalClip(QPointF(0.0, 1.0), 17.0);
        ClipInfo downWithMotionRotation =
            makeDirectionalClip(QPointF(0.0, 1.0), 0.0);
        KeyframeTrack rotation(QStringLiteral("motion.rotation"), 0.0);
        rotation.addKeyframe(0.0, 11.0);
        rotation.addKeyframe(1.0, 11.0);
        downWithMotionRotation.keyframes.addTrack(rotation);
        const double rightAngle =
            clipanim::effectiveTransformAt(right, 0.5).rotationDeg;
        const double downAngle =
            clipanim::effectiveTransformAt(down, 0.5).rotationDeg;
        const double offsetAngle =
            clipanim::effectiveTransformAt(downWithOffset, 0.5).rotationDeg;
        const double motionOffsetAngle = clipanim::effectiveTransformAt(
            downWithMotionRotation, 0.5).rotationDeg;
        check(8, "auto-orient follows right/down paths and adds rotation offset",
              near(rightAngle, 0.0, 0.5)
                  && near(downAngle, 90.0, 0.5)
                  && near(offsetAngle, 107.0, 0.5)
                  && near(motionOffsetAngle, 101.0, 0.5),
              QStringLiteral("right=%1 down=%2 staticOffset=%3 motionOffset=%4")
                  .arg(rightAngle, 0, 'g', 12)
                  .arg(downAngle, 0, 'g', 12)
                  .arg(offsetAngle, 0, 'g', 12)
                  .arg(motionOffsetAngle, 0, 'g', 12));
    }

    // G9: disabling auto-orient restores every transform component bit-for-bit.
    {
        ClipInfo clip = makeClip();
        clip.videoScale = 1.25;
        clip.videoDx = -0.125;
        clip.videoDy = 0.375;
        clip.rotation2DDegrees = -23.5;
        KeyframeTrack x(kPosXTrack, clip.videoDx);
        x.addKeyframe(0.0, -0.125);
        x.addKeyframe(1.0, 0.875);
        KeyframeTrack y(kPosYTrack, clip.videoDy);
        y.addKeyframe(0.0, 0.375);
        y.addKeyframe(1.0, 0.875);
        clip.keyframes.addTrack(x);
        clip.keyframes.addTrack(y);

        clip.autoOrientEnabled = false;
        const clipgeom::ClipTransform before =
            clipanim::effectiveTransformAt(clip, 0.375);
        clip.autoOrientEnabled = true;
        const clipgeom::ClipTransform oriented =
            clipanim::effectiveTransformAt(clip, 0.375);
        clip.autoOrientEnabled = false;
        const clipgeom::ClipTransform after =
            clipanim::effectiveTransformAt(clip, 0.375);
        const bool bitIdentical = sameBits(before.videoScale, after.videoScale)
            && sameBits(before.videoDx, after.videoDx)
            && sameBits(before.videoDy, after.videoDy)
            && sameBits(before.rotationDeg, after.rotationDeg);
        check(9, "auto-orient OFF leaves effective transform bit-identical",
              bitIdentical && !sameBits(before.rotationDeg, oriented.rotationDeg),
              QStringLiteral("before=%1 oriented=%2 after=%3")
                  .arg(before.rotationDeg, 0, 'g', 17)
                  .arg(oriented.rotationDeg, 0, 'g', 17)
                  .arg(after.rotationDeg, 0, 'g', 17));
    }

    std::printf("[spatial-path] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
