#pragma once

#include "../ClipGeometry.h"
#include "../VideoEffect.h"

#include <QPointF>

struct ClipInfo;

namespace clipanim {

// Returns effective 2D position at clip-local time.
// If the surrounding position segment has no spatial tangents, this is the
// same independent per-axis keyframe result used by effectiveTransformAt().
QPointF effectivePositionAt(const ClipInfo& clip,
                            double clipLocalSeconds);

// Returns effective transform at clip-local time.
// If no keyframe track exists for a property, uses the static ClipInfo value.
// If all motion tracks are absent/empty, returns the static values unchanged.
clipgeom::ClipTransform effectiveTransformAt(const ClipInfo& clip,
                                             double clipLocalSeconds);

// Returns effective opacity. If motion.opacity track is absent/empty, returns
// staticOpacity unchanged.
double effectiveOpacityAt(const ClipInfo& clip, double clipLocalSeconds,
                          double staticOpacity);

// Returns effective effect parameters at clip-local time.
// If no effect.* keyframe tracks exist, returns the static effect stack
// unchanged.
QVector<VideoEffect> effectiveEffectsAt(const ClipInfo& clip,
                                        double clipLocalSeconds);

} // namespace clipanim
