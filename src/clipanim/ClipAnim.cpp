#include "ClipAnim.h"

#include "../EffectParamSchema.h"
#include "../Keyframe.h"
#include "../Timeline.h"

namespace clipanim {
namespace {

const QString kScaleTrack = QStringLiteral("motion.scale");
const QString kPosXTrack = QStringLiteral("motion.position.x");
const QString kPosYTrack = QStringLiteral("motion.position.y");
const QString kRotationTrack = QStringLiteral("motion.rotation");
const QString kOpacityTrack = QStringLiteral("motion.opacity");

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

} // namespace

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
    if (trackHasKeyframes(clip.keyframes, kPosXTrack)) {
        transform.videoDx =
            clip.keyframes.valueAt(kPosXTrack, clipLocalSeconds, transform.videoDx);
    }
    if (trackHasKeyframes(clip.keyframes, kPosYTrack)) {
        transform.videoDy =
            clip.keyframes.valueAt(kPosYTrack, clipLocalSeconds, transform.videoDy);
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
