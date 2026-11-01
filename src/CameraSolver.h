#pragma once

#include <QPolygonF>
#include <QSize>
#include <QVector>
#include <QVector3D>
#include <limits>

struct Camera3DState;
class Camera3D;

namespace camsolve {

// Pinhole intrinsics in pixels; independent of Camera3D's legacy fov semantics.
struct Intrinsics { double f; double cx, cy; };
Intrinsics makeIntrinsics(double fovDeg, QSize canvas);
// Camera3DState::fov is already a focal length in pixels.
Intrinsics makeIntrinsicsFromFocalPx(double fPx, QSize canvas);

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
void applyPosesToCamera(Camera3D& camera, const QVector<Pose>& poses,
                        double fps, double startSec);

// Feature-solve mapping onto the project camera (canvas px). unitScalePx (S) scales solver
// units to px; focalPx is the analysis-image focal length; referenceIndex is the pose the
// solve is centred on. legacy marks the planar path, which keeps applyPosesToCamera.
struct SolveMapping {
    double unitScalePx = 0;
    double focalPx = 0;
    QSize analysisSize;
    QSize canvasSize;
    int referenceIndex = -1;
    bool legacy = false;
    // The solved range [startSec, startSec + sampleCount/fps]: sampleCount counts frames, solved
    // or not. sampleCount 0 means it was not recorded (a project saved before this field).
    double startSec = 0;
    int sampleCount = 0;
    double fps = 0;
};
struct CameraSolveApplied {
    QVector<Pose> poses;
    double fps = 0;
    double startSec = 0;
    SolveMapping mapping;
    QVector<QVector3D> points; // solver units
};

// Canvas focal length f = focalPx / max(aw/cw, ah/ch) (the inverse of the dialog's focal).
double canvasFocalPx(const SolveMapping& mapping);
// First valid pose, or -1.
int referencePoseIndex(const QVector<Pose>& poses);
// Re-expresses poses and points in the reference camera frame (the reference pose becomes
// identity at the origin). Returns false and changes nothing when refIndex is not a valid pose.
bool normaliseToReference(QVector<Pose>& poses, QVector<QVector3D>& points, int refIndex);
// f / median(z) over the reference-frame points with z > 0, unrounded; f when there is none.
double defaultUnitScale(const QVector<QVector3D>& normalisedPoints, double f);
// Camera at a normalised pose: position = S*c + f*(R3 - e3), forward = -R3, fov = f, so the
// pinhole (position + f*forward) is S*c - f*e3 and the reference pose is the default camera.
Camera3DState poseToProjectCameraState(const Pose& normalised, const Camera3DState& base,
                                       const SolveMapping& mapping);
// World px of a normalised solver point: S*p - f*e3.
QVector3D solvePointToWorldPx(const QVector3D& normalisedPoint, const SolveMapping& mapping);
// Centres on the first valid pose, keyframes every valid pose with fov = f (the Fov track is
// replaced) and stores the mapping (referenceIndex = that pose). Rejects focalPx <= 0, a
// non-finite f / S or S <= 0, no valid pose, or a bad fps / startSec: camera unchanged, false.
bool applySolvedPosesToCamera(Camera3D& camera, const CameraSolveApplied& applied);

} // namespace camsolve
