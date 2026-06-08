#pragma once

#include "../ClipGeometry.h"

struct ClipInfo;

namespace clipanim {

// Returns effective transform at clip-local time.
// If no keyframe track exists for a property, uses the static ClipInfo value.
// If all motion tracks are absent/empty, returns the static values unchanged.
clipgeom::ClipTransform effectiveTransformAt(const ClipInfo& clip,
                                             double clipLocalSeconds);

// Returns effective opacity. If motion.opacity track is absent/empty, returns
// staticOpacity unchanged.
double effectiveOpacityAt(const ClipInfo& clip, double clipLocalSeconds,
                          double staticOpacity);

} // namespace clipanim
