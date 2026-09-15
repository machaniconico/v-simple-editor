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

// Returns the direction of travel along the position path. Cubic spatial
// handles use the Bezier derivative; ordinary position keyframes use the
// adjacent-keyframe direction. Returns false when no direction is available.
bool spatialTangentAt(const ClipInfo& clip,
                      double clipLocalSeconds,
                      QPointF *tangent);

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

// Returns effective per-clip color correction at clip-local time.
// If no grade.* keyframe tracks exist, returns the static ColorCorrection
// unchanged.
ColorCorrection effectiveColorCorrectionAt(const ClipInfo& clip,
                                           double clipLocalSeconds);

// Shared descriptors keep evaluation, section insertion and persistence names aligned.
struct HslGradeTrack { QString name; double HslSecondaryGrade::*member; };
struct PrimaryGradeTrack { QString name; double ColorCorrection::*member; };
struct WarpGradeTrack { QString name; int ring; int hue; bool shift; };
const QVector<HslGradeTrack>& hslGradeTracks();
const QVector<PrimaryGradeTrack>& sectionGradeTracks(bool log);
const QVector<WarpGradeTrack>& warpGradeTracks();
// Acceptance instrumentation: bypass only the newly added evaluation branches.
void setExtendedGradeDisabledForTest(bool disabled);
void resetExtendedGradeCallCountForTest();
int extendedGradeCallCountForTest();
bool hasHslSecondaryKeyframes(const ClipInfo& clip);
HslSecondaryGrade effectiveHslSecondaryAt(const ClipInfo& clip, double localSec);

} // namespace clipanim
