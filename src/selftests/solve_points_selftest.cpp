#include "../BundleAdjustment.h"
#include "../Camera3D.h"
#include "../MainWindow.h"
#include "../PlanarTrackerDialog.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace {
// Reach the production apply / undo path without widening MainWindow's API.
template<class Tag, typename Tag::Type Member> struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
#define ACCESS(Name, TypeExpr, MemberExpr) \
    struct Name { using Type = TypeExpr; friend Type member(Name); }; \
    template struct Access<Name, MemberExpr>;
ACCESS(Apply, void (MainWindow::*)(const camsolve::CameraSolveApplied&), &MainWindow::applyCameraSolve)
ACCESS(Undo, void (MainWindow::*)(), &MainWindow::undoAction)
ACCESS(Camera, Camera3D MainWindow::*, &MainWindow::m_projectCamera)
#undef ACCESS

// Noise-free, nonplanar 64-point / 10-frame scene with a nonuniform camera arc.
sfm::MultiViewTracks scene()
{
    sfm::MultiViewTracks tracks;
    tracks.frameCount = 10;
    QVector<camsolve::Pose> truth;
    for (int frame = 0; frame < tracks.frameCount; ++frame) {
        const double angle = .04*frame + .003*frame*frame;
        const double c = std::cos(angle), s = std::sin(angle);
        const double center[3] = {6*s, .2*std::sin(2*angle), 6*(1-c)};
        camsolve::Pose p;
        const double rotation[9] = {c,0,s, 0,1,0, -s,0,c};
        std::copy(rotation, rotation+9, p.R);
        for (int row = 0; row < 3; ++row) {
            double t = 0;
            for (int col = 0; col < 3; ++col) t -= p.R[3*row+col]*center[col];
            p.t[row] = float(t);
        }
        p.valid = true;
        truth.append(p);
    }
    std::mt19937 rng(48213);
    const auto uniform = [&] { return (double(rng())+.5)/4294967296.0; };
    for (int id = 0; id < 64; ++id) {
        const double point[3] = {3*uniform()-1.5, 2*uniform()-1, 4+4*uniform()};
        sfm::Track2D track;
        track.pointId = id;
        track.strength = 64-id;
        for (int frame = 0; frame < tracks.frameCount; ++frame) {
            const auto& p = truth[frame];
            double projected[3] = {};
            for (int row = 0; row < 3; ++row) {
                projected[row] = p.t[row];
                for (int col = 0; col < 3; ++col) projected[row] += p.R[3*row+col]*point[col];
            }
            track.observations.append(qMakePair(frame,
                QPointF(800*projected[0]/projected[2]+640, 800*projected[1]/projected[2]+360)));
        }
        tracks.tracks.append(track);
    }
    return tracks;
}
}

int runSolvePointsSelftest()
{
    selftest::GateTally tally;
    const auto tracks = scene();
    MainWindow window;
    auto& camera = window.*member(Camera{});
    const auto before = camera.toJson();
    PlanarTrackerDialog dialog;
    dialog.setCameraSolveContext({1280,720}, 800, 24, 5);
    dialog.setFeatureTrackingSource("synthetic", 24, 240);
    dialog.setFeatureTrackingResult(tracks, {1280,720}, 0);
    int applySignals = 0;
    camsolve::CameraSolveApplied emitted;
    QObject::connect(&dialog, &PlanarTrackerDialog::cameraSolveApplied, &window,
        [&](const camsolve::CameraSolveApplied& applied) {
            ++applySignals; emitted = applied;
            (window.*member(Apply{}))(applied);
        });

    // G1: the solved cloud reaches the project camera, survives JSON, and is absent when empty.
    const bool solved = dialog.solveFeatureCamera();
    const auto cloud = dialog.featureSolveResult().bundle.points;
    const auto firstCloud = camera.solvePoints();
    const bool carried = solved && applySignals == 1 && cloud.size() > 0
        && camera.solvePoints().size() == cloud.size();
    Camera3D restored;
    const auto json = camera.toJson();
    restored.fromJson(json);
    bool roundTrip = restored.solvePoints().size() == camera.solvePoints().size()
        && restored.toJson() == json;
    for (qsizetype i = 0; roundTrip && i < restored.solvePoints().size(); ++i)
        roundTrip = (restored.solvePoints()[i] - camera.solvePoints()[i]).length() < 1e-5f;
    // The cloud is stored centred on the reference camera, not as the raw solver output.
    auto centredPoses = emitted.poses;
    auto centred = emitted.points;
    camsolve::normaliseToReference(centredPoses, centred, camsolve::referencePoseIndex(centredPoses));
    bool centredOk = centred.size() == camera.solvePoints().size();
    for (qsizetype i = 0; centredOk && i < centred.size(); ++i)
        centredOk = (centred[i] - camera.solvePoints()[i]).length() < 1e-4f;
    const auto world = camera.solvePointsWorldPx();
    const auto mapping = camera.solveMapping();
    // Hand-written expected value (not the production helper): world = S*p - (0,0,F), with a
    // sanity band on S and F and the analysis size equal to the canvas so canvas focal == focalPx.
    const double mapS = mapping ? mapping->unitScalePx : 0.0, mapF = mapping ? mapping->focalPx : 0.0;
    bool worldOk = mapping && mapS > 0 && mapF > 0 && mapping->analysisSize == mapping->canvasSize
        && world.size() == camera.solvePoints().size();
    for (qsizetype i = 0; worldOk && i < world.size(); ++i) {
        const QVector3D p = camera.solvePoints()[i];
        const QVector3D expected(float(mapS*p.x()), float(mapS*p.y()), float(mapS*p.z() - mapF));
        worldOk = (world[i] - expected).length() < 1e-3f;
    }
    // Control: a camera with a mapping but no cloud writes no key and has no world points.
    Camera3D bare;
    bare.setSolveMapping(mapping);
    const bool keyAbsent = !bare.toJson().contains(QStringLiteral("solvePoints"))
        && bare.solvePointsWorldPx().isEmpty() && !before.contains(QStringLiteral("solvePoints"))
        && json.contains(QStringLiteral("solvePoints"))
        && json[QStringLiteral("solvePoints")].toArray().size() == int(cloud.size()) * 3;
    std::fprintf(stderr, "G1 solved=%d applySignals=%d cloud=%lld stored=%lld carried=%d roundTrip=%d centred=%d world=%d S=%.6f F=%.6f keyAbsent=%d\n",
        solved, applySignals, static_cast<long long>(cloud.size()),
        static_cast<long long>(camera.solvePoints().size()), carried, roundTrip, centredOk, worldOk, mapS, mapF, keyAbsent);
    tally.gate(1, carried && roundTrip && centredOk && worldOk && keyAbsent
               && cloud.size() <= sfm::kMaxBundlePoints);

    // G3 direct: a feature re-solve with no planar apply in between replaces (not appends to) the cloud.
    const auto firstEmitted = emitted;
    auto subset = tracks;
    subset.tracks.resize(40);
    dialog.setFeatureTrackingResult(subset, {1280,720}, 0);
    const bool directResolved = dialog.solveFeatureCamera();
    const auto directCloud = dialog.featureSolveResult().bundle.points;
    auto directPoses = emitted.poses;
    auto directCentred = emitted.points;
    camsolve::normaliseToReference(directPoses, directCentred, camsolve::referencePoseIndex(directPoses));
    bool directReplaced = directResolved && applySignals == 2 && !directCloud.isEmpty()
        && directCloud.size() < firstCloud.size()
        && camera.solvePoints().size() == directCloud.size()
        && directCentred.size() == camera.solvePoints().size();
    for (qsizetype i = 0; directReplaced && i < directCentred.size(); ++i)
        directReplaced = (directCentred[i] - camera.solvePoints()[i]).length() < 1e-4f;
    // G2 nonempty undo: restores the first solve's camera, cloud included, through fromJson.
    (window.*member(Undo{}))();
    const bool undoneNonEmpty = camera.toJson() == json && camera.solvePoints().size() == firstCloud.size();

    // G3 part 1 runs before undo: a planar solve empties the cloud it replaces.
    camsolve::CameraSolveApplied planar;
    planar.poses = firstEmitted.poses;
    planar.fps = firstEmitted.fps;
    planar.startSec = firstEmitted.startSec;
    planar.mapping.legacy = true;
    (window.*member(Apply{}))(planar);
    const bool planarEmpty = camera.solvePoints().isEmpty() && !camera.solveMapping()
        && !camera.toJson().contains(QStringLiteral("solvePoints")) && !firstCloud.isEmpty();
    // A feature re-solve on a subset of tracks replaces the cloud with the new one.
    dialog.setFeatureTrackingResult(subset, {1280,720}, 0);
    const bool resolved = dialog.solveFeatureCamera();
    const auto secondCloud = dialog.featureSolveResult().bundle.points;
    const bool replaced = resolved && applySignals == 3 && secondCloud.size() > 0
        && secondCloud.size() < firstCloud.size()
        && camera.solvePoints().size() == secondCloud.size();
    std::fprintf(stderr, "G3 directReplaced=%d direct=%lld planarEmpty=%d resolved=%d first=%lld second=%lld stored=%lld replaced=%d\n",
        directReplaced, static_cast<long long>(directCloud.size()), planarEmpty, resolved, static_cast<long long>(firstCloud.size()),
        static_cast<long long>(secondCloud.size()),
        static_cast<long long>(camera.solvePoints().size()), replaced);

    // G2: one undo restores the camera JSON from before the (last) solve, cloud included.
    (window.*member(Undo{}))();
    const bool undone = camera.solvePoints().isEmpty()
        && !camera.toJson().contains(QStringLiteral("solvePoints"));
    // Undo of the very first solve: start again from the original state.
    MainWindow fresh;
    auto& freshCamera = fresh.*member(Camera{});
    const auto freshBefore = freshCamera.toJson();
    (fresh.*member(Apply{}))(firstEmitted);
    const bool freshChanged = freshCamera.toJson() != freshBefore && !freshCamera.solvePoints().isEmpty();
    (fresh.*member(Undo{}))();
    const bool freshRestored = freshCamera.toJson() == freshBefore && freshCamera.solvePoints().isEmpty();
    // Loading a cloud-free JSON into a camera that holds a cloud drops the old cloud.
    Camera3D reused;
    reused.fromJson(json);
    const bool hadCloud = !reused.solvePoints().isEmpty();
    reused.fromJson(before);
    const bool reloadCleared = hadCloud && reused.solvePoints().isEmpty();
    std::fprintf(stderr, "G2 undoneNonEmpty=%d undoneToPlanar=%d freshChanged=%d freshRestored=%d reloadCleared=%d\n",
        undoneNonEmpty, undone, freshChanged, freshRestored, reloadCleared);
    tally.gate(2, undoneNonEmpty && undone && freshChanged && freshRestored && reloadCleared);
    tally.gate(3, directReplaced && planarEmpty && replaced);
    return tally.finish();
}
