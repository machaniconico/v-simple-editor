#include "ClipAnim.h"

#include "../EasingCurveModel.h"
#include "../EffectParamSchema.h"
#include "../Keyframe.h"
#include "../Timeline.h"

#include <algorithm>
#include <cmath>

namespace clipanim {
namespace {

const QString kScaleTrack = QStringLiteral("motion.scale");
const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");
const QString kRotationTrack = QStringLiteral("motion.rotation");
const QString kOpacityTrack = QStringLiteral("motion.opacity");

constexpr double kKeyTimeEpsilon = 1e-6;

bool trackHasKeyframes(const KeyframeManager& keyframes, const QString& trackName)
{
    if (!keyframes.hasTrack(trackName))
        return false;
    const KeyframeTrack *track = keyframes.track(trackName);
    return track && track->count() > 0;
}

bool hasAnyMotionKeyframes(const ClipInfo& clip)
{
    return trackHasKeyframes(clip.keyframes, kScaleTrack)
        || trackHasKeyframes(clip.keyframes, kPosXTrack)
        || trackHasKeyframes(clip.keyframes, kPosYTrack)
        || trackHasKeyframes(clip.keyframes, kRotationTrack)
        || trackHasKeyframes(clip.keyframes, kOpacityTrack);
}

bool hasAnyEffectKeyframes(const ClipInfo& clip)
{
    for (const KeyframeTrack &track : clip.keyframes.tracks()) {
        if (track.propertyName().startsWith(QStringLiteral("effect."))
            && track.count() > 0) {
            return true;
        }
    }
    return false;
}

bool hasAnyEffectTiming(const ClipInfo& clip)
{
    for (const VideoEffect &effect : clip.effects) {
        if (effect.startSec >= 0.0 || effect.endSec >= 0.0)
            return true;
    }
    return false;
}

bool effectActiveAt(const VideoEffect& effect, double clipLocalSeconds)
{
    if (effect.startSec >= 0.0 && clipLocalSeconds < effect.startSec)
        return false;
    if (effect.endSec >= 0.0 && clipLocalSeconds > effect.endSec)
        return false;
    return true;
}

bool finitePoint(const QPointF& p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

bool hasUsableSpatialTangent(const KeyframePoint *kf)
{
    return kf
        && kf->hasSpatialTangent
        && std::isfinite(kf->spatialOutX)
        && std::isfinite(kf->spatialOutY)
        && std::isfinite(kf->spatialInX)
        && std::isfinite(kf->spatialInY);
}

const KeyframePoint *keyframeAtTime(const KeyframeTrack *track, double time)
{
    if (!track)
        return nullptr;
    for (const KeyframePoint &kf : track->keyframes()) {
        if (std::abs(kf.time - time) <= kKeyTimeEpsilon)
            return &kf;
    }
    return nullptr;
}

void appendKeyframeTimes(QVector<double>& times, const KeyframeTrack *track)
{
    if (!track)
        return;
    for (const KeyframePoint &kf : track->keyframes())
        times.append(kf.time);
}

QVector<double> positionKeyframeTimes(const KeyframeTrack *xTrack,
                                      const KeyframeTrack *yTrack)
{
    QVector<double> times;
    appendKeyframeTimes(times, xTrack);
    appendKeyframeTimes(times, yTrack);
    std::sort(times.begin(), times.end());

    QVector<double> uniqueTimes;
    for (double time : times) {
        if (uniqueTimes.isEmpty()
            || std::abs(uniqueTimes.last() - time) > kKeyTimeEpsilon) {
            uniqueTimes.append(time);
        }
    }
    return uniqueTimes;
}

double trackValueAt(const KeyframeTrack *track, double time, double defaultValue)
{
    if (!track || track->count() == 0)
        return defaultValue;
    return track->valueAt(time);
}

double easedProgress(double t, const KeyframePoint *kf)
{
    const KeyframePoint::Interpolation interpolation =
        kf ? kf->interpolation : KeyframePoint::Linear;
    switch (interpolation) {
    case KeyframePoint::Linear:
        return t;
    case KeyframePoint::EaseIn:
        return t * t;
    case KeyframePoint::EaseOut:
        return t * (2.0 - t);
    case KeyframePoint::EaseInOut:
        return t < 0.5 ? 2.0 * t * t : -1.0 + (4.0 - 2.0 * t) * t;
    case KeyframePoint::Hold:
        return 0.0;
    case KeyframePoint::Bezier: {
        const easing::CubicBezier bez{
            kf ? kf->bezX1 : 0.0,
            kf ? kf->bezY1 : 0.0,
            kf ? kf->bezX2 : 1.0,
            kf ? kf->bezY2 : 1.0
        };
        return easing::evaluate(easing::EasingType::CubicBezier, t, bez);
    }
    case KeyframePoint::ElasticOut:
        return easing::elasticOut(t);
    case KeyframePoint::BounceOut:
        return easing::bounceOut(t);
    case KeyframePoint::BackOut:
        return easing::backOut(t);
    }
    return t;
}

QPointF outgoingSpatialTangent(const KeyframePoint *xKf,
                               const KeyframePoint *yKf)
{
    if (hasUsableSpatialTangent(xKf))
        return QPointF(xKf->spatialOutX, xKf->spatialOutY);
    if (hasUsableSpatialTangent(yKf))
        return QPointF(yKf->spatialOutX, yKf->spatialOutY);
    return QPointF(0.0, 0.0);
}

QPointF incomingSpatialTangent(const KeyframePoint *xKf,
                               const KeyframePoint *yKf)
{
    if (hasUsableSpatialTangent(xKf))
        return QPointF(xKf->spatialInX, xKf->spatialInY);
    if (hasUsableSpatialTangent(yKf))
        return QPointF(yKf->spatialInX, yKf->spatialInY);
    return QPointF(0.0, 0.0);
}

QPointF cubicPoint(const QPointF& p0,
                   const QPointF& c1,
                   const QPointF& c2,
                   const QPointF& p1,
                   double u)
{
    const double omt = 1.0 - u;
    const double a = omt * omt * omt;
    const double b = 3.0 * omt * omt * u;
    const double c = 3.0 * omt * u * u;
    const double d = u * u * u;
    return QPointF(a * p0.x() + b * c1.x() + c * c2.x() + d * p1.x(),
                   a * p0.y() + b * c1.y() + c * c2.y() + d * p1.y());
}

bool spatialPositionAt(const ClipInfo& clip,
                       double clipLocalSeconds,
                       QPointF *position)
{
    const KeyframeTrack *xTrack = clip.keyframes.track(kPosXTrack);
    const KeyframeTrack *yTrack = clip.keyframes.track(kPosYTrack);
    const QVector<double> times = positionKeyframeTimes(xTrack, yTrack);
    if (times.size() < 2)
        return false;

    int segment = -1;
    for (int i = 0; i < times.size() - 1; ++i) {
        if (clipLocalSeconds + kKeyTimeEpsilon >= times[i]
            && clipLocalSeconds - kKeyTimeEpsilon <= times[i + 1]) {
            segment = i;
            break;
        }
    }
    if (segment < 0)
        return false;

    const double startTime = times[segment];
    const double endTime = times[segment + 1];
    if (!(endTime > startTime))
        return false;

    const KeyframePoint *xStart = keyframeAtTime(xTrack, startTime);
    const KeyframePoint *yStart = keyframeAtTime(yTrack, startTime);
    const KeyframePoint *xEnd = keyframeAtTime(xTrack, endTime);
    const KeyframePoint *yEnd = keyframeAtTime(yTrack, endTime);

    if (!hasUsableSpatialTangent(xStart)
        && !hasUsableSpatialTangent(yStart)
        && !hasUsableSpatialTangent(xEnd)
        && !hasUsableSpatialTangent(yEnd)) {
        return false;
    }

    QPointF p0(trackValueAt(xTrack, startTime, clip.videoDx),
               trackValueAt(yTrack, startTime, clip.videoDy));
    QPointF p1(trackValueAt(xTrack, endTime, clip.videoDx),
               trackValueAt(yTrack, endTime, clip.videoDy));
    if (!finitePoint(p0) || !finitePoint(p1))
        return false;

    const QPointF out = outgoingSpatialTangent(xStart, yStart);
    const QPointF in = incomingSpatialTangent(xEnd, yEnd);
    const QPointF c1(p0.x() + out.x(), p0.y() + out.y());
    const QPointF c2(p1.x() + in.x(), p1.y() + in.y());
    if (!finitePoint(c1) || !finitePoint(c2))
        return false;

    double u = (clipLocalSeconds - startTime) / (endTime - startTime);
    u = std::max(0.0, std::min(1.0, u));
    const KeyframePoint *easeKeyframe = xStart ? xStart : yStart;
    u = easedProgress(u, easeKeyframe);

    const QPointF result = cubicPoint(p0, c1, c2, p1, u);
    if (!finitePoint(result))
        return false;

    if (position)
        *position = result;
    return true;
}

} // namespace

QPointF effectivePositionAt(const ClipInfo& clip,
                            double clipLocalSeconds)
{
    QPointF spatialPosition;
    if (spatialPositionAt(clip, clipLocalSeconds, &spatialPosition))
        return spatialPosition;

    QPointF position(clip.videoDx, clip.videoDy);
    if (trackHasKeyframes(clip.keyframes, kPosXTrack)) {
        position.setX(
            clip.keyframes.valueAt(kPosXTrack, clipLocalSeconds, position.x()));
    }
    if (trackHasKeyframes(clip.keyframes, kPosYTrack)) {
        position.setY(
            clip.keyframes.valueAt(kPosYTrack, clipLocalSeconds, position.y()));
    }
    return position;
}

clipgeom::ClipTransform effectiveTransformAt(const ClipInfo& clip,
                                             double clipLocalSeconds)
{
    // Hot path: most clips have no motion keyframes. Return before any
    // valueAt() call so static clips keep the legacy values byte-for-byte.
    if (!hasAnyMotionKeyframes(clip)) {
        return clipgeom::ClipTransform{clip.videoScale, clip.videoDx,
                                       clip.videoDy, clip.rotation2DDegrees};
    }

    clipgeom::ClipTransform transform{clip.videoScale, clip.videoDx,
                                      clip.videoDy, clip.rotation2DDegrees};
    if (trackHasKeyframes(clip.keyframes, kScaleTrack)) {
        transform.videoScale =
            clip.keyframes.valueAt(kScaleTrack, clipLocalSeconds, transform.videoScale);
    }
    QPointF spatialPosition;
    if (spatialPositionAt(clip, clipLocalSeconds, &spatialPosition)) {
        transform.videoDx = spatialPosition.x();
        transform.videoDy = spatialPosition.y();
    } else {
        if (trackHasKeyframes(clip.keyframes, kPosXTrack)) {
            transform.videoDx =
                clip.keyframes.valueAt(kPosXTrack, clipLocalSeconds, transform.videoDx);
        }
        if (trackHasKeyframes(clip.keyframes, kPosYTrack)) {
            transform.videoDy =
                clip.keyframes.valueAt(kPosYTrack, clipLocalSeconds, transform.videoDy);
        }
    }
    if (trackHasKeyframes(clip.keyframes, kRotationTrack)) {
        transform.rotationDeg =
            clip.keyframes.valueAt(kRotationTrack, clipLocalSeconds, transform.rotationDeg);
    }
    return transform;
}

double effectiveOpacityAt(const ClipInfo& clip, double clipLocalSeconds,
                          double staticOpacity)
{
    // Same first-branch fast path as transform evaluation. The opacity helper
    // still checks all five motion tracks so empty/static clips avoid valueAt().
    if (!hasAnyMotionKeyframes(clip))
        return staticOpacity;
    if (!trackHasKeyframes(clip.keyframes, kOpacityTrack))
        return staticOpacity;
    return clip.keyframes.valueAt(kOpacityTrack, clipLocalSeconds, staticOpacity);
}

QVector<VideoEffect> effectiveEffectsAt(const ClipInfo& clip,
                                        double clipLocalSeconds)
{
    // Hot path: most clips have no effect-parameter keyframes. Return before
    // copying/mapping parameters so the static FX path keeps legacy values
    // byte-for-byte.
    const bool hasEffectKeyframes = hasAnyEffectKeyframes(clip);
    const bool hasEffectTiming = hasAnyEffectTiming(clip);
    if (!hasEffectKeyframes && !hasEffectTiming)
        return clip.effects;

    QVector<VideoEffect> effects = clip.effects;
    for (int i = 0; i < effects.size(); ++i) {
        if (!hasEffectKeyframes)
            continue;
        const auto schema = effectctrl::paramSchemaFor(effects[i].type);
        for (const auto &def : schema) {
            const QString trackName =
                QStringLiteral("effect.%1.%2").arg(i).arg(def.name);
            if (!trackHasKeyframes(clip.keyframes, trackName))
                continue;

            const double currentValue =
                effectctrl::paramValue(effects[i], def.name);
            const double value =
                clip.keyframes.valueAt(trackName, clipLocalSeconds, currentValue);
            effectctrl::setParamValue(effects[i], def.name, value);
        }
    }
    if (hasEffectTiming) {
        QVector<VideoEffect> filtered;
        filtered.reserve(effects.size());
        for (const VideoEffect &effect : effects) {
            if (effectActiveAt(effect, clipLocalSeconds))
                filtered.append(effect);
        }
        return filtered;
    }
    return effects;
}

} // namespace clipanim
