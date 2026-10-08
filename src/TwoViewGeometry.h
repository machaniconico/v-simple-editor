#pragma once

#include "CameraSolver.h"
#include <QPointF>
#include <QVector>
#include <QVector3D>

namespace sfm {
struct Correspondence { QPointF a, b; };
struct TwoViewResult {
    double R[9] = {1,0,0, 0,1,0, 0,0,1};
    double t[3] = {0,0,0};
    double E[9] = {};
    QVector<bool> inliers;
    int inlierCount = 0;
    double meanReprojErr = 0; // Mean Euclidean pixel error over both views.
    double medianParallaxDeg = 0;
    bool valid = false;
};
// World frame is camera A; camera B uses X_b = R X_a + t.
// Valid results have |t| = 1; invalid results carry no usable translation.
TwoViewResult estimateRelativePose(const QVector<Correspondence>& px,
    const camsolve::Intrinsics& K, double ransacThresholdPx = 1.0,
    int ransacIters = 500, unsigned seed = 12345);
// Output preserves input order. Failed/infinite points are represented by NaNs.
QVector<QVector3D> triangulate(const QVector<Correspondence>& px,
    const camsolve::Intrinsics& K, const double R[9], const double t[3]);
// Mean error in pixels; invalid points/inputs yield infinity.
double reprojectionError(const Correspondence& px, const QVector3D& point,
    const camsolve::Intrinsics& K, const double R[9], const double t[3]);
} // namespace sfm
