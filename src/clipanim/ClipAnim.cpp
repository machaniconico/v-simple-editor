#include "ClipAnim.h"

#include "../EasingCurveModel.h"
#include "../EffectParamSchema.h"
#include "../Keyframe.h"
#include "../Timeline.h"

#include <QColor>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace clipanim {
namespace {

const QString kScaleTrack = QStringLiteral("motion.scale");
const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");
// Dynamic Zoom stores only the public TransformAnimator track names; the
// runtime motion.* names are resolved from them at read time so no mirrored
// tracks are ever persisted. motion.* stays authoritative when present.
const QString kPublicPosXTrack = QStringLiteral("positionX");
const QString kPublicPosYTrack = QStringLiteral("positionY");
const QString kPublicScaleXTrack = QStringLiteral("scaleX");
const QString kPublicScaleYTrack = QStringLiteral("scaleY");
const QString kRotationTrack = QStringLiteral("motion.rotation");
const QString kOpacityTrack = QStringLiteral("motion.opacity");
const QString kGradeBrightnessTrack = QStringLiteral("grade.brightness");
const QString kGradeContrastTrack = QStringLiteral("grade.contrast");
const QString kGradeSaturationTrack = QStringLiteral("grade.saturation");
const QString kGradeExposureTrack = QStringLiteral("grade.exposure");
const QString kGradeTemperatureTrack = QStringLiteral("grade.temperature");
const QString kGradeLiftRTrack = QStringLiteral("grade.liftR");
const QString kGradeLiftGTrack = QStringLiteral("grade.liftG");
const QString kGradeLiftBTrack = QStringLiteral("grade.liftB");
const QString kGradeGammaRTrack = QStringLiteral("grade.gammaR");
const QString kGradeGammaGTrack = QStringLiteral("grade.gammaG");
const QString kGradeGammaBTrack = QStringLiteral("grade.gammaB");
const QString kGradeGainRTrack = QStringLiteral("grade.gainR");
const QString kGradeGainGTrack = QStringLiteral("grade.gainG");
const QString kGradeGainBTrack = QStringLiteral("grade.gainB");

std::atomic<bool> extendedGradeDisabled{false};
std::atomic<int> extendedGradeCalls{0};

constexpr double kKeyTimeEpsilon = 1e-6;
constexpr double kColorChannelMin = 0.0;
constexpr double kColorChannelMax = 255.0;

bool trackHasKeyframes(const KeyframeManager& keyframes, const QString& trackName)
{
    if (!keyframes.hasTrack(trackName))
        return false;
    const KeyframeTrack *track = keyframes.track(trackName);
    return track && track->count() > 0;
}

// Runtime-name resolution: prefer the motion.* track, otherwise fall back to
// the public TransformAnimator name Dynamic Zoom stores. Returning the runtime
// name when neither has keyframes keeps every "no keyframes" branch untouched.
const QString& resolvedMotionTrack(const KeyframeManager& keyframes,
                                   const QString& runtimeName,
                                   const QString& publicName)
{
    if (trackHasKeyframes(keyframes, runtimeName))
        return runtimeName;
    if (trackHasKeyframes(keyframes, publicName))
        return publicName;
    return runtimeName;
}

// motion.scale is a uniform scale; Dynamic Zoom writes identical scaleX and
// scaleY tracks, so either public track can stand in for it.
const QString& resolvedScaleTrack(const KeyframeManager& keyframes)
{
    if (trackHasKeyframes(keyframes, kScaleTrack))
        return kScaleTrack;
    if (trackHasKeyframes(keyframes, kPublicScaleXTrack))
        return kPublicScaleXTrack;
    if (trackHasKeyframes(keyframes, kPublicScaleYTrack))
        return kPublicScaleYTrack;
    return kScaleTrack;
}

bool hasAnyMotionKeyframes(const ClipInfo& clip)
{
    return trackHasKeyframes(clip.keyframes, kScaleTrack)
        || trackHasKeyframes(clip.keyframes, kPosXTrack)
        || trackHasKeyframes(clip.keyframes, kPosYTrack)
        || trackHasKeyframes(clip.keyframes, kRotationTrack)
        || trackHasKeyframes(clip.keyframes, kOpacityTrack)
        || trackHasKeyframes(clip.keyframes, kPublicScaleXTrack)
        || trackHasKeyframes(clip.keyframes, kPublicScaleYTrack)
        || trackHasKeyframes(clip.keyframes, kPublicPosXTrack)
        || trackHasKeyframes(clip.keyframes, kPublicPosYTrack);
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

bool hasAnyGradeKeyframes(const ClipInfo& clip)
{
    for (const KeyframeTrack &track : clip.keyframes.tracks()) {
        if (track.propertyName().startsWith(QStringLiteral("grade.")) && track.count() > 0)
            return true;
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

void applyGradeTrackValue(const ClipInfo& clip,
                          const QString& trackName,
                          double clipLocalSeconds,
                          double& value)
{
    if (!trackHasKeyframes(clip.keyframes, trackName))
        return;
    value = clip.keyframes.valueAt(trackName, clipLocalSeconds, value);
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

QPointF cubicTangent(const QPointF& p0,
                     const QPointF& c1,
                     const QPointF& c2,
                     const QPointF& p1,
                     double u)
{
    const double omt = 1.0 - u;
    return 3.0 * omt * omt * (c1 - p0)
        + 6.0 * omt * u * (c2 - c1)
        + 3.0 * u * u * (p1 - c2);
}

struct SpatialSegmentEvaluation {
    QPointF p0;
    QPointF c1;
    QPointF c2;
    QPointF p1;
    double u = 0.0;
    bool hasSpatialTangents = false;
};

bool evaluateSpatialSegment(const ClipInfo& clip,
                            const KeyframeTrack *xTrack,
                            const KeyframeTrack *yTrack,
                            const QVector<double>& times,
                            double pathSeconds,
                            SpatialSegmentEvaluation *evaluation)
{
    int segment = -1;
    for (int i = 0; i < times.size() - 1; ++i) {
        if (pathSeconds + kKeyTimeEpsilon >= times[i]
            && pathSeconds - kKeyTimeEpsilon <= times[i + 1]) {
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

    SpatialSegmentEvaluation result;
    result.hasSpatialTangents = hasUsableSpatialTangent(xStart)
        || hasUsableSpatialTangent(yStart)
        || hasUsableSpatialTangent(xEnd)
        || hasUsableSpatialTangent(yEnd);
    result.p0 = QPointF(trackValueAt(xTrack, startTime, clip.videoDx),
                        trackValueAt(yTrack, startTime, clip.videoDy));
    result.p1 = QPointF(trackValueAt(xTrack, endTime, clip.videoDx),
                        trackValueAt(yTrack, endTime, clip.videoDy));
    if (!finitePoint(result.p0) || !finitePoint(result.p1))
        return false;

    const QPointF out = outgoingSpatialTangent(xStart, yStart);
    const QPointF in = incomingSpatialTangent(xEnd, yEnd);
    result.c1 = result.p0 + out;
    result.c2 = result.p1 + in;
    if (!finitePoint(result.c1) || !finitePoint(result.c2))
        return false;

    result.u = (pathSeconds - startTime) / (endTime - startTime);
    result.u = std::max(0.0, std::min(1.0, result.u));
    const KeyframePoint *easeKeyframe = xStart ? xStart : yStart;
    result.u = easedProgress(result.u, easeKeyframe);
    if (!std::isfinite(result.u))
        return false;

    if (evaluation)
        *evaluation = result;
    return true;
}

bool spatialLoopApplies(const ClipInfo& clip, const QString& trackName)
{
    const LoopMode mode = clip.keyframes.loopOutMode(trackName);
    const KeyframeTrack *track = clip.keyframes.track(trackName);
    return (mode == LoopMode::Cycle || mode == LoopMode::PingPong)
        && track
        && track->count() >= 2;
}

double spatialPathLocalSecondsForTrack(const ClipInfo& clip,
                                       const QString& trackName,
                                       double clipLocalSeconds)
{
    if (spatialLoopApplies(clip, trackName))
        return clip.keyframes.loopedTimeForTrack(trackName, clipLocalSeconds);
    return clipLocalSeconds;
}

double spatialPathDirectionForTrack(const ClipInfo& clip,
                                    const QString& trackName,
                                    double clipLocalSeconds)
{
    if (clip.keyframes.loopOutMode(trackName) != LoopMode::PingPong)
        return 1.0;
    const KeyframeTrack *track = clip.keyframes.track(trackName);
    if (!track || track->count() < 2)
        return 1.0;
    const QVector<KeyframePoint>& keyframes = track->keyframes();
    const double firstTime = keyframes.first().time;
    const double lastTime = keyframes.last().time;
    const double range = lastTime - firstTime;
    if (clipLocalSeconds <= lastTime || !std::isfinite(range) || range <= 0.0)
        return 1.0;
    double phase = std::fmod(clipLocalSeconds - firstTime, 2.0 * range);
    if (phase < 0.0)
        phase += 2.0 * range;
    return phase > range ? -1.0 : 1.0;
}

double clampedAnimatedParamValue(const effectctrl::ParamDef& def, double value)
{
    if (def.type != effectctrl::ParamType::Int
        && def.type != effectctrl::ParamType::Choice) {
        return value;
    }

    if (!std::isfinite(value))
        return def.defaultVal;

    double minValue = def.minVal;
    double maxValue = def.maxVal;
    if ((!std::isfinite(minValue)
         || !std::isfinite(maxValue)
         || maxValue < minValue)
        && !def.choices.isEmpty()) {
        minValue = 0.0;
        maxValue = static_cast<double>(def.choices.size() - 1);
    }

    if (std::isfinite(minValue)
        && std::isfinite(maxValue)
        && maxValue >= minValue) {
        return std::max(minValue, std::min(maxValue, value));
    }
    return value;
}

QString effectParamTrackName(int effectIndex, const QString& paramName)
{
    return QStringLiteral("effect.%1.%2").arg(effectIndex).arg(paramName);
}

QString effectColorChannelTrackName(int effectIndex,
                                    const QString& paramName,
                                    const QString& channel)
{
    return QStringLiteral("effect.%1.%2.%3")
        .arg(effectIndex)
        .arg(paramName)
        .arg(channel);
}

int clampedColorChannel(double value, int defaultValue)
{
    if (!std::isfinite(value))
        return defaultValue;
    const double clamped = std::max(kColorChannelMin,
                                    std::min(kColorChannelMax, value));
    return static_cast<int>(std::lround(clamped));
}

int colorChannelValueAt(const ClipInfo& clip,
                        const QString& trackName,
                        double clipLocalSeconds,
                        int currentValue)
{
    if (!trackHasKeyframes(clip.keyframes, trackName))
        return currentValue;
    return clampedColorChannel(
        clip.keyframes.valueAt(trackName, clipLocalSeconds, currentValue),
        currentValue);
}

bool applyAnimatedColorParam(VideoEffect& effect,
                             const effectctrl::ParamDef& def,
                             const ClipInfo& clip,
                             int effectIndex,
                             double clipLocalSeconds)
{
    const QString rTrack = effectColorChannelTrackName(
        effectIndex, def.name, QStringLiteral("r"));
    const QString gTrack = effectColorChannelTrackName(
        effectIndex, def.name, QStringLiteral("g"));
    const QString bTrack = effectColorChannelTrackName(
        effectIndex, def.name, QStringLiteral("b"));
    if (!trackHasKeyframes(clip.keyframes, rTrack)
        && !trackHasKeyframes(clip.keyframes, gTrack)
        && !trackHasKeyframes(clip.keyframes, bTrack)) {
        return false;
    }

    const QColor current = effectctrl::colorParamValue(effect, def.name);
    const QColor animated(
        colorChannelValueAt(clip, rTrack, clipLocalSeconds, current.red()),
        colorChannelValueAt(clip, gTrack, clipLocalSeconds, current.green()),
        colorChannelValueAt(clip, bTrack, clipLocalSeconds, current.blue()),
        current.alpha());
    effectctrl::setColorParam(effect, def.name, animated);
    return true;
}

bool spatialPositionAt(const ClipInfo& clip,
                       double clipLocalSeconds,
                       QPointF *position)
{
    const QString& posXName =
        resolvedMotionTrack(clip.keyframes, kPosXTrack, kPublicPosXTrack);
    const QString& posYName =
        resolvedMotionTrack(clip.keyframes, kPosYTrack, kPublicPosYTrack);
    const KeyframeTrack *xTrack = clip.keyframes.track(posXName);
    const KeyframeTrack *yTrack = clip.keyframes.track(posYName);
    const QVector<double> times = positionKeyframeTimes(xTrack, yTrack);
    if (times.size() < 2)
        return false;

    const auto evaluateSpatialPointAt = [&](double pathSeconds,
                                             QPointF *result) {
        SpatialSegmentEvaluation evaluation;
        if (!evaluateSpatialSegment(clip, xTrack, yTrack, times,
                                    pathSeconds, &evaluation)
            || !evaluation.hasSpatialTangents) {
            return false;
        }
        const QPointF evaluated = cubicPoint(
            evaluation.p0, evaluation.c1, evaluation.c2, evaluation.p1,
            evaluation.u);
        if (!finitePoint(evaluated))
            return false;
        if (result)
            *result = evaluated;
        return true;
    };

    // Position X and Y are independently selectable graph tracks.  Their
    // Loop Out modes therefore need independent time mappings even when the
    // pair uses one spatial Bezier path.  The previous shared mapping picked
    // X first and made Y visibly loop despite a UI value of None/PingPong.
    QPointF result(
        trackHasKeyframes(clip.keyframes, posXName)
            ? clip.keyframes.valueAt(posXName, clipLocalSeconds, clip.videoDx)
            : clip.videoDx,
        trackHasKeyframes(clip.keyframes, posYName)
            ? clip.keyframes.valueAt(posYName, clipLocalSeconds, clip.videoDy)
            : clip.videoDy);

    QPointF xPoint;
    QPointF yPoint;
    const bool xSpatial = evaluateSpatialPointAt(
        spatialPathLocalSecondsForTrack(clip, posXName, clipLocalSeconds),
        &xPoint);
    const bool ySpatial = evaluateSpatialPointAt(
        spatialPathLocalSecondsForTrack(clip, posYName, clipLocalSeconds),
        &yPoint);
    if (xSpatial)
        result.setX(xPoint.x());
    if (ySpatial)
        result.setY(yPoint.y());

    if (position)
        *position = result;
    return xSpatial || ySpatial;
}

} // namespace

QPointF effectivePositionAt(const ClipInfo& clip,
                            double clipLocalSeconds)
{
    QPointF spatialPosition;
    if (spatialPositionAt(clip, clipLocalSeconds, &spatialPosition))
        return spatialPosition;

    QPointF position(clip.videoDx, clip.videoDy);
    const QString& posXName =
        resolvedMotionTrack(clip.keyframes, kPosXTrack, kPublicPosXTrack);
    const QString& posYName =
        resolvedMotionTrack(clip.keyframes, kPosYTrack, kPublicPosYTrack);
    if (trackHasKeyframes(clip.keyframes, posXName)) {
        position.setX(
            clip.keyframes.valueAt(posXName, clipLocalSeconds, position.x()));
    }
    if (trackHasKeyframes(clip.keyframes, posYName)) {
        position.setY(
            clip.keyframes.valueAt(posYName, clipLocalSeconds, position.y()));
    }
    return position;
}

bool spatialTangentAt(const ClipInfo& clip,
                      double clipLocalSeconds,
                      QPointF *tangent)
{
    const QString& posXName =
        resolvedMotionTrack(clip.keyframes, kPosXTrack, kPublicPosXTrack);
    const QString& posYName =
        resolvedMotionTrack(clip.keyframes, kPosYTrack, kPublicPosYTrack);
    const KeyframeTrack *xTrack = clip.keyframes.track(posXName);
    const KeyframeTrack *yTrack = clip.keyframes.track(posYName);
    const QVector<double> times = positionKeyframeTimes(xTrack, yTrack);
    if (times.size() < 2)
        return false;

    const auto tangentPathSeconds = [&](const QString& trackName) {
        return std::max(times.first(), std::min(
            times.last(), spatialPathLocalSecondsForTrack(
                clip, trackName, clipLocalSeconds)));
    };

    SpatialSegmentEvaluation xEvaluation;
    SpatialSegmentEvaluation yEvaluation;
    const bool hasXEvaluation = evaluateSpatialSegment(
        clip, xTrack, yTrack, times,
        tangentPathSeconds(posXName),
        &xEvaluation);
    const bool hasYEvaluation = evaluateSpatialSegment(
        clip, xTrack, yTrack, times,
        tangentPathSeconds(posYName),
        &yEvaluation);
    if (!hasXEvaluation && !hasYEvaluation)
        return false;

    const auto segmentTangent = [](const SpatialSegmentEvaluation& evaluation) {
        if (evaluation.hasSpatialTangents) {
            const QPointF derivative = cubicTangent(
                evaluation.p0, evaluation.c1, evaluation.c2, evaluation.p1,
                evaluation.u);
            if (finitePoint(derivative)
                && (derivative.x() != 0.0 || derivative.y() != 0.0)) {
                return derivative;
            }
        }
        return evaluation.p1 - evaluation.p0;
    };

    const QPointF xDirection = hasXEvaluation
        ? segmentTangent(xEvaluation) : QPointF();
    const QPointF yDirection = hasYEvaluation
        ? segmentTangent(yEvaluation) : QPointF();
    const QPointF result(
        xDirection.x() * spatialPathDirectionForTrack(
            clip, posXName, clipLocalSeconds),
        yDirection.y() * spatialPathDirectionForTrack(
            clip, posYName, clipLocalSeconds));
    if (!finitePoint(result) || (result.x() == 0.0 && result.y() == 0.0))
        return false;
    if (tangent)
        *tangent = result;
    return true;
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
    const QString& scaleName = resolvedScaleTrack(clip.keyframes);
    if (trackHasKeyframes(clip.keyframes, scaleName)) {
        transform.videoScale =
            clip.keyframes.valueAt(scaleName, clipLocalSeconds, transform.videoScale);
    }
    QPointF spatialPosition;
    if (spatialPositionAt(clip, clipLocalSeconds, &spatialPosition)) {
        transform.videoDx = spatialPosition.x();
        transform.videoDy = spatialPosition.y();
    } else {
        const QString& posXName =
            resolvedMotionTrack(clip.keyframes, kPosXTrack, kPublicPosXTrack);
        const QString& posYName =
            resolvedMotionTrack(clip.keyframes, kPosYTrack, kPublicPosYTrack);
        if (trackHasKeyframes(clip.keyframes, posXName)) {
            transform.videoDx =
                clip.keyframes.valueAt(posXName, clipLocalSeconds, transform.videoDx);
        }
        if (trackHasKeyframes(clip.keyframes, posYName)) {
            transform.videoDy =
                clip.keyframes.valueAt(posYName, clipLocalSeconds, transform.videoDy);
        }
    }
    if (trackHasKeyframes(clip.keyframes, kRotationTrack)) {
        transform.rotationDeg =
            clip.keyframes.valueAt(kRotationTrack, clipLocalSeconds, transform.rotationDeg);
    }
    if (clip.autoOrientEnabled) {
        QPointF tangent;
        if (spatialTangentAt(clip, clipLocalSeconds, &tangent)) {
            constexpr double kRadiansToDegrees =
                180.0 / 3.14159265358979323846264338327950288;
            transform.rotationDeg += std::atan2(tangent.y(), tangent.x())
                * kRadiansToDegrees;
        }
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
            if (def.type == effectctrl::ParamType::Color) {
                applyAnimatedColorParam(effects[i], def, clip, i, clipLocalSeconds);
                continue;
            }
            const QString trackName = effectParamTrackName(i, def.name);
            if (!trackHasKeyframes(clip.keyframes, trackName))
                continue;

            const double currentValue =
                effectctrl::paramValue(effects[i], def.name);
            const double interpolated =
                clip.keyframes.valueAt(trackName, clipLocalSeconds, currentValue);
            const double value = clampedAnimatedParamValue(def, interpolated);
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

ColorCorrection effectiveColorCorrectionAt(const ClipInfo& clip,
                                           double clipLocalSeconds)
{
    // Hot path: static grade clips keep the exact existing ColorCorrection
    // values and avoid valueAt() entirely.
    if (!hasAnyGradeKeyframes(clip))
        return clip.colorCorrection;

    ColorCorrection cc = clip.colorCorrection;
    applyGradeTrackValue(clip, kGradeBrightnessTrack,
                         clipLocalSeconds, cc.brightness);
    applyGradeTrackValue(clip, kGradeContrastTrack,
                         clipLocalSeconds, cc.contrast);
    applyGradeTrackValue(clip, kGradeSaturationTrack,
                         clipLocalSeconds, cc.saturation);
    applyGradeTrackValue(clip, kGradeExposureTrack,
                         clipLocalSeconds, cc.exposure);
    applyGradeTrackValue(clip, kGradeTemperatureTrack,
                         clipLocalSeconds, cc.temperature);

    applyGradeTrackValue(clip, kGradeLiftRTrack,
                         clipLocalSeconds, cc.liftR);
    applyGradeTrackValue(clip, kGradeLiftGTrack,
                         clipLocalSeconds, cc.liftG);
    applyGradeTrackValue(clip, kGradeLiftBTrack,
                         clipLocalSeconds, cc.liftB);
    applyGradeTrackValue(clip, kGradeGammaRTrack,
                         clipLocalSeconds, cc.gammaR);
    applyGradeTrackValue(clip, kGradeGammaGTrack,
                         clipLocalSeconds, cc.gammaG);
    applyGradeTrackValue(clip, kGradeGammaBTrack,
                         clipLocalSeconds, cc.gammaB);
    applyGradeTrackValue(clip, kGradeGainRTrack,
                         clipLocalSeconds, cc.gainR);
    applyGradeTrackValue(clip, kGradeGainGTrack,
                         clipLocalSeconds, cc.gainG);
    applyGradeTrackValue(clip, kGradeGainBTrack,
                         clipLocalSeconds, cc.gainB);
    if (extendedGradeDisabled.load(std::memory_order_relaxed)) return cc;
    for (const auto &track : sectionGradeTracks(true)) {
        if (!trackHasKeyframes(clip.keyframes, track.name)) continue;
        ++extendedGradeCalls;
        applyGradeTrackValue(clip, track.name, clipLocalSeconds, cc.*(track.member));
    }
    for (const auto &track : warpGradeTracks()) {
        if (!trackHasKeyframes(clip.keyframes, track.name)) continue;
        ++extendedGradeCalls;
        float &value = track.shift ? cc.hueSatWarp.hueShiftDeg[track.ring][track.hue]
                                   : cc.hueSatWarp.satScale[track.ring][track.hue];
        value = static_cast<float>(clip.keyframes.valueAt(track.name, clipLocalSeconds, value));
    }
    return cc;
}

const QVector<HslGradeTrack>& hslGradeTracks()
{
    static const QVector<HslGradeTrack> tracks = {
        {QStringLiteral("grade.hsl.hueCenter"), &HslSecondaryGrade::hueCenter},
        {QStringLiteral("grade.hsl.hueRange"), &HslSecondaryGrade::hueRange},
        {QStringLiteral("grade.hsl.satMin"), &HslSecondaryGrade::satMin},
        {QStringLiteral("grade.hsl.satMax"), &HslSecondaryGrade::satMax},
        {QStringLiteral("grade.hsl.lumaMin"), &HslSecondaryGrade::lumaMin},
        {QStringLiteral("grade.hsl.lumaMax"), &HslSecondaryGrade::lumaMax},
        {QStringLiteral("grade.hsl.softness"), &HslSecondaryGrade::softness},
        {QStringLiteral("grade.hsl.liftR"), &HslSecondaryGrade::liftR},
        {QStringLiteral("grade.hsl.liftG"), &HslSecondaryGrade::liftG},
        {QStringLiteral("grade.hsl.liftB"), &HslSecondaryGrade::liftB},
        {QStringLiteral("grade.hsl.gammaR"), &HslSecondaryGrade::gammaR},
        {QStringLiteral("grade.hsl.gammaG"), &HslSecondaryGrade::gammaG},
        {QStringLiteral("grade.hsl.gammaB"), &HslSecondaryGrade::gammaB},
        {QStringLiteral("grade.hsl.gainR"), &HslSecondaryGrade::gainR},
        {QStringLiteral("grade.hsl.gainG"), &HslSecondaryGrade::gainG},
        {QStringLiteral("grade.hsl.gainB"), &HslSecondaryGrade::gainB},
    };
    return tracks;
}

const QVector<PrimaryGradeTrack>& sectionGradeTracks(bool log)
{
    static const QVector<PrimaryGradeTrack> lgg = {
        {QStringLiteral("grade.liftR"), &ColorCorrection::liftR},
        {QStringLiteral("grade.liftG"), &ColorCorrection::liftG},
        {QStringLiteral("grade.liftB"), &ColorCorrection::liftB},
        {QStringLiteral("grade.gammaR"), &ColorCorrection::gammaR},
        {QStringLiteral("grade.gammaG"), &ColorCorrection::gammaG},
        {QStringLiteral("grade.gammaB"), &ColorCorrection::gammaB},
        {QStringLiteral("grade.gainR"), &ColorCorrection::gainR},
        {QStringLiteral("grade.gainG"), &ColorCorrection::gainG},
        {QStringLiteral("grade.gainB"), &ColorCorrection::gainB},
    };
    static const QVector<PrimaryGradeTrack> logs = {
        {QStringLiteral("grade.logShadowR"), &ColorCorrection::logShadowR},
        {QStringLiteral("grade.logShadowG"), &ColorCorrection::logShadowG},
        {QStringLiteral("grade.logShadowB"), &ColorCorrection::logShadowB},
        {QStringLiteral("grade.logMidR"), &ColorCorrection::logMidR},
        {QStringLiteral("grade.logMidG"), &ColorCorrection::logMidG},
        {QStringLiteral("grade.logMidB"), &ColorCorrection::logMidB},
        {QStringLiteral("grade.logHighR"), &ColorCorrection::logHighR},
        {QStringLiteral("grade.logHighG"), &ColorCorrection::logHighG},
        {QStringLiteral("grade.logHighB"), &ColorCorrection::logHighB},
    };
    return log ? logs : lgg;
}

const QVector<WarpGradeTrack>& warpGradeTracks()
{
    static const QVector<WarpGradeTrack> tracks = [] {
        QVector<WarpGradeTrack> result;
        for (int ring = 0; ring < HueSatWarp::kSatRings; ++ring)
            for (int hue = 0; hue < HueSatWarp::kHueNodes; ++hue)
                for (bool shift : {true, false})
                    result.append({QStringLiteral("grade.hueSatWarp.%1.%2.%3")
                        .arg(shift ? QStringLiteral("shift") : QStringLiteral("scale"))
                        .arg(ring).arg(hue), ring, hue, shift});
        return result;
    }();
    return tracks;
}

bool hasHslSecondaryKeyframes(const ClipInfo& clip)
{
    for (const auto &track : clip.keyframes.tracks())
        if (track.propertyName().startsWith(QStringLiteral("grade.hsl.")) && track.count() > 0)
            return true;
    return false;
}

HslSecondaryGrade effectiveHslSecondaryAt(const ClipInfo& clip, double localSec)
{
    HslSecondaryGrade hsl = clip.hslSecondary;
    if (extendedGradeDisabled.load(std::memory_order_relaxed)
        || !hasHslSecondaryKeyframes(clip)) return hsl;
    ++extendedGradeCalls;
    for (const auto &track : hslGradeTracks())
        applyGradeTrackValue(clip, track.name, localSec, hsl.*(track.member));
    return hsl;
}

void setExtendedGradeDisabledForTest(bool disabled) { extendedGradeDisabled.store(disabled); }
void resetExtendedGradeCallCountForTest() { extendedGradeCalls.store(0); }
int extendedGradeCallCountForTest() { return extendedGradeCalls.load(); }

} // namespace clipanim
