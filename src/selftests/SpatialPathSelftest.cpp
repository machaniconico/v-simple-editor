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

bool finite(double value)
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
              finite(pos.y()) && std::abs(pos.y()) > 0.1,
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
              finite(oneSidedMid.x())
                  && finite(oneSidedMid.y())
                  && finite(degenerateMid.x())
                  && finite(degenerateMid.y())
                  && exactly(degenerateMid.x(), 0.5)
                  && exactly(degenerateMid.y(), 0.0)
                  && finite(invalidMid.x())
                  && finite(invalidMid.y())
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

    std::printf("[spatial-path] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
