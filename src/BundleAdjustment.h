#pragma once

#include "CameraSolver.h"
#include <QImage>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QVector>
#include <limits>

namespace sfm {

inline constexpr int kMaxBundleFrames = 60;
inline constexpr int kMaxBundlePoints = 800;

// A is n*n, row-major. On failure x is empty; inputs are not modified.
bool solveDenseLinearSystem(QVector<double> A, QVector<double> b, int n,
                            QVector<double>& x);

struct Track2D {
    int pointId = -1;
    QVector<QPair<int, QPointF>> observations;
    double strength = 0; // Shi-Tomasi rank; higher is stronger.
};

struct MultiViewTracks {
    int frameCount = 0;
    QVector<Track2D> tracks;
};

// Detect only in frame 0. Rejected tracks never restart or gain new observations.
MultiViewTracks trackAcrossFrames(const QVector<QImage>& frames, int maxPoints = kMaxBundlePoints);

// World-to-camera poses, X_camera = R * X_world + t, with frame 0 identity.
// Monocular scale is fixed by making the first baseline one unit. Subsequent
// baselines use median shared-point depth ratios in the intervening camera.
// Always returns frameCount entries (empty for nonpositive frameCount).
// Failure, including insufficient shared scale points, invalidates the suffix.
QVector<camsolve::Pose> chainTwoViewPoses(const MultiViewTracks& tracks,
                                         const camsolve::Intrinsics& intrinsics);

struct BundleResult {
    // Linear solver the iterations entered; None when no solve was attempted
    // (rejected input, maxIters == 0, or an initial cost already at zero).
    enum class Backend { None, Dense, Schur };
    Backend backend = Backend::None;
    bool valid = false;
    bool converged = false;
    QString reason;
    QVector<camsolve::Pose> poses;
    QVector<QVector3D> points;
    double rms = std::numeric_limits<double>::infinity();
    // Initial RMS followed by accepted double-precision iterations, in pixels
    // per observation. rms also includes final QVector3D float rounding.
    QVector<double> rmsHistory;
};

// Points correspond to tracks in vector order, not pointId. Frame 0 is fixed;
// absolute monocular scale is unobservable (no metric scale is recovered).
// Rotations use local axis-angle increments. Schur elimination is the default.
// Tests only: VEDITOR_BUNDLE_DENSE_SELFTEST=1 selects the preserved dense oracle.
BundleResult bundleAdjust(const MultiViewTracks& tracks,
                          QVector<camsolve::Pose> initialPoses,
                          QVector<QVector3D> initialPoints,
                          const camsolve::Intrinsics& intrinsics, int maxIters = 20);
// Retains original sample indices so sparse solves can use the existing camera API.
struct CameraTrackSolve {
    MultiViewTracks input;
    QVector<int> sourceFrames;
    bool thinned = false;
    BundleResult bundle;
};
CameraTrackSolve sampleCameraTracks(const MultiViewTracks& tracks);
CameraTrackSolve solveCameraTracks(const MultiViewTracks& tracks,
                                  const camsolve::Intrinsics& intrinsics);

// World-to-camera poses suitable for camsolve::applyPosesToCamera.
QVector<camsolve::Pose> toCameraPoses(const BundleResult& result);

} // namespace sfm
