#pragma once

// RotoTracking — shape propagation for rotoscoped paths
//
// Estimates a 2-D similarity transform (translation + rotation + uniform scale)
// between consecutive frames by NCC-matching a sparse set of sample points
// sampled around the seed RotoPath, then least-squares-fitting the similarity
// from the matched correspondences.  The accumulated transform from the seed
// frame is applied to the seed path to produce the path at each frame.
//
// Handle convention in applyTransformToPath():
//   handleIn / handleOut are ABSOLUTE bezier control-point coordinates
//   (see Rotoscope.cpp: pathToQPainterPath uses cubicTo(handleOut, handleIn,
//   position) directly).  Therefore they are mapped through the full xf,
//   exactly like position:
//       dst.handleIn  = xf.map(src.handleIn);
//       dst.handleOut = xf.map(src.handleOut);
//   For identity:  handle → handle  [unchanged].
//   For pure translation T:  handle → handle + T  [shifts by same translation].

#include "Rotoscope.h"

#include <QImage>
#include <QTransform>
#include <QVector>

namespace rototrack {

// --- Tuning parameters for propagateRotoShape ---

struct RotoTrackParams {
    // Emit a RotoKeyframe every this many frames (plus seed + last).
    int keyframeInterval = 5;

    // Reject a frame's transform if uniform scale deviates from 1.0 by more
    // than this fraction (e.g. 0.25 → scale outside [0.75, 1.25] is rejected).
    double maxScaleDelta = 0.25;

    // Half-width of the NCC search window around each sample point (pixels).
    int searchMargin = 24;

    // Minimum NCC score [0, 1] required to accept a point correspondence.
    double minConfidence = 0.4;
};

// --- Core API ---

// Transform a RotoPath by xf.
//   - Each point's position, handleIn, and handleOut are all mapped through
//     xf as absolute points (handles are bezier control-point coordinates).
// Postconditions:
//   identity xf  → returned path compares equal to the input path.
//   pure translation T → every position and handle shifts by T.
RotoPath applyTransformToPath(const RotoPath &path, const QTransform &xf);

// Propagate seedPath forward (and backward if seedFrame > firstFrameIndex)
// through the frame sequence using NCC template matching + similarity fitting.
//
// Parameters:
//   seedPath        — the roto shape at seedFrame (source of truth).
//   seedFrame       — absolute frame number of seedPath within the project.
//   frames          — contiguous sequence of QImages.
//   firstFrameIndex — absolute frame number of frames[0].
//   p               — tuning parameters.
//
// Returns a QVector<RotoKeyframe> with:
//   - One keyframe at seedFrame.
//   - One keyframe every p.keyframeInterval frames after the seed (forward).
//   - One keyframe at the last frame (if not already covered by the interval).
//   - Frame numbers are strictly increasing.
//
// Robustness guarantees:
//   - Returns {seedFrame, seedPath} on empty / single-frame sequences.
//   - Null / empty QImages are skipped (previous transform reused).
//   - If fewer than 2 good correspondences survive confidence filtering, or
//     the fitted scale deviates by more than p.maxScaleDelta, the previous
//     cumulative transform is reused for that frame (shape held).
QVector<RotoKeyframe> propagateRotoShape(const RotoPath &seedPath,
                                         int seedFrame,
                                         const QVector<QImage> &frames,
                                         int firstFrameIndex,
                                         const RotoTrackParams &p);

} // namespace rototrack
