#pragma once

#include "CameraSolver.h"
#include <QImage>
#include <QPair>
#include <QPointF>
#include <QVector>

namespace sfm {

// A is n*n, row-major. On failure x is empty; inputs are not modified.
bool solveDenseLinearSystem(QVector<double> A, QVector<double> b, int n,
                            QVector<double>& x);

struct Track2D {
    int pointId = -1;
    QVector<QPair<int, QPointF>> observations;
};

struct MultiViewTracks {
    int frameCount = 0;
    QVector<Track2D> tracks;
};

// Detect only in frame 0. Rejected tracks never restart or gain new observations.
MultiViewTracks trackAcrossFrames(const QVector<QImage>& frames, int maxPoints = 150);

// World-to-camera poses, X_camera = R * X_world + t, with frame 0 identity.
// Monocular scale is fixed by making the first baseline one unit. Subsequent
// baselines use median shared-point depth ratios in the intervening camera.
// Always returns frameCount entries (empty for nonpositive frameCount).
// Failure, including insufficient shared scale points, invalidates the suffix.
QVector<camsolve::Pose> chainTwoViewPoses(const MultiViewTracks& tracks,
                                         const camsolve::Intrinsics& intrinsics);

} // namespace sfm
