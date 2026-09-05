#pragma once

#include <QPolygonF>
#include <QSize>
#include <QVector>
#include <QVector3D>
#include <limits>

struct Camera3DState;

namespace camsolve {

// Pinhole intrinsics in pixels; independent of Camera3D's legacy fov semantics.
struct Intrinsics { double f; double cx, cy; };
Intrinsics makeIntrinsics(double fovDeg, QSize canvas);

struct Pose {
    double R[9] = {1,0,0, 0,1,0, 0,0,1}; // row-major, world to camera
    QVector3D t;
    QVector3D n = QVector3D(0, 0, -1);
    bool valid = false;
    double residual = std::numeric_limits<double>::infinity(); // Frobenius reconstruction error
};

Pose decomposeHomography(const double H[9], const Intrinsics& intrinsics,
                         const Pose* previous = nullptr);
QVector<Pose> solveSequence(const QVector<QPolygonF>& cornersPerFrame,
                            const QVector<double>& confidencePerFrame,
                            int refFrame, const Intrinsics& intrinsics,
                            double confidenceThreshold = 0.5);
Camera3DState poseToCameraState(const Pose& pose, const Camera3DState& base);

} // namespace camsolve
