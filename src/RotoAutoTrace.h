#pragma once

// RotoAutoTrace — edge-contour auto-trace and edge-snap utilities
// US-PRO-1: free functions in namespace rototrace
//
// Dependencies: Qt6 (QImage, QPointF, QRectF), <cmath>, <algorithm>, <vector>
// No third-party deps (no OpenCV).
//
// CMake wiring is deferred to a later story; add src/RotoAutoTrace.cpp to
// CMakeLists.txt target sources when ready.

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include "Rotoscope.h"  // RotoPoint, RotoPath

namespace rototrace {

/// Parameters for autoTraceContour().
struct RotoAutoTraceParams {
    double blurRadius      = 1.5;   ///< Gaussian pre-blur sigma (pixels); 0 = no blur
    double edgeThreshold   = 40.0;  ///< Sobel gradient magnitude threshold [0..255*sqrt2]
    int    maxPoints       = 40;    ///< Maximum RotoPoints in result path
    double simplifyEpsilon = 2.0;   ///< Ramer-Douglas-Peucker epsilon (pixels)
};

/// Find the dominant closed contour inside seedRegion and return it as a
/// closed RotoPath with smooth Catmull-Rom handles.
///
/// Algorithm:
///   1. Convert frame to greyscale.
///   2. Optional Gaussian blur (sigma = p.blurRadius, box-blur approximation).
///   3. Sobel gradient magnitude.
///   4. Threshold at p.edgeThreshold to produce a binary edge image.
///   5. Moore-neighbour boundary trace starting from the first edge pixel
///      found along the seedRegion's left edge (scanline search).
///   6. Ramer-Douglas-Peucker polyline simplification (p.simplifyEpsilon).
///   7. Trim to p.maxPoints if still over limit.
///   8. Fit smooth in/out handles (Catmull-Rom, 1/3 segment length).
///
/// Fallback: if no edge pixel is found within seedRegion, returns a closed
/// RotoPath with the four corners of seedRegion and straight handles (zero).
/// This is documented behaviour — callers should test path.points.size() == 4
/// to distinguish the fallback.
///
/// Guaranteed: terminates and does not crash on empty/null/1×1 images.
RotoPath autoTraceContour(const QImage &frame,
                          const QRectF &seedRegion,
                          const RotoAutoTraceParams &p = RotoAutoTraceParams{});

/// Within searchRadius pixels of p, return the pixel location with the
/// strongest Sobel gradient magnitude.
///
/// Sub-pixel refinement: parabolic fit on the 3×3 neighbourhood around the
/// peak pixel. If the gradient at the input point p is already the local
/// maximum (or searchRadius < 1), p is returned unchanged.
///
/// Guarantee: gradient magnitude at the returned point >= magnitude at p.
QPointF snapPointToEdge(const QImage &frame,
                        QPointF p,
                        double searchRadius = 12.0,
                        double blurRadius   = 1.0);

/// Return a same-size grayscale (Format_Grayscale8) edge-map image.
/// Pixel values are proportional to Sobel gradient magnitude after optional
/// blur; values below threshold are set to 0, others are clamped to [0,255].
/// Passing threshold = 0 returns the raw magnitude map (no thresholding).
/// Deterministic: same inputs always produce identical output.
QImage debugEdgeMap(const QImage &frame,
                    double blurRadius,
                    double threshold);

}  // namespace rototrace
