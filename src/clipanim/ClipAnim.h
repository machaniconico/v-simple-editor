#pragma once

#include "../ClipGeometry.h"
#include "../VideoEffect.h"

#include <QPointF>
#include <QSize>

#include <functional>

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

// Returns effective transform at clip-local time from the keyframes only
// (and autoOrient): wiggle is NOT included. The paths that write the result
// back into static values (arrange / parenting / SurfaceTool) use this form.
// If no keyframe track exists for a property, uses the static ClipInfo value.
// If all motion tracks are absent/empty, returns the static values unchanged.
clipgeom::ClipTransform effectiveTransformAt(const ClipInfo& clip,
                                             double clipLocalSeconds);

// Returns effective opacity from the keyframes only. If motion.opacity track
// is absent/empty, returns staticOpacity unchanged.
double effectiveOpacityAt(const ClipInfo& clip, double clipLocalSeconds,
                          double staticOpacity);

// US-1504: what the dynamic (non-keyframe) overlay needs from the caller.
// canvas is the PROJECT canvas (Timeline::projectOutputSize(); the special
// composite passes its canvasSize), never the output / display size, so a
// pixel amplitude keeps its normalized magnitude at any export resolution.
// audioSamplerFor is reserved for expression bindings (empty = level 0).
struct EvalEnv {
    double fps = 30.0;
    QSize canvas;
    std::function<std::function<double(double)>(const ClipInfo&, double clipStart)> audioSamplerFor;
};

// True when the clip has motion keyframes or an enabled wiggle: the drawing
// callers gate the env evaluation on this predicate.
bool hasAnyDynamicTransform(const ClipInfo& clip);

// The single dynamic overlay applied on top of the keyframed values (in the
// order keyframes -> autoOrient -> overlay): wiggle adds its pixel offset,
// converted to normalized canvas fractions through env.canvas, its rotation
// and its scale multiplier. Null pointers are skipped.
void applyDynamicOverlay(const ClipInfo& clip, double clipLocalSeconds,
                         const EvalEnv& env, double *posX, double *posY,
                         double *scale, double *rotationDeg, double *opacity01);

// Drawing forms: keyframes -> autoOrient -> applyDynamicOverlay. Identical to
// the two-argument forms when hasAnyDynamicTransform(clip) is false.
clipgeom::ClipTransform effectiveTransformAt(const ClipInfo& clip,
                                             double clipLocalSeconds,
                                             const EvalEnv& env);
double effectiveOpacityAt(const ClipInfo& clip, double clipLocalSeconds,
                          double staticOpacity, const EvalEnv& env);

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
