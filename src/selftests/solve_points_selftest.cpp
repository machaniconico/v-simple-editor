#include "../BundleAdjustment.h"
#include "../Camera3D.h"
#include "../GLPreview.h"
#include "../MainWindow.h"
#include "../PlanarTrackerDialog.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "SelftestGate.h"

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QStatusBar>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
ACCESS(Sync, void (MainWindow::*)(), &MainWindow::syncProjectLightingToTimeline)
ACCESS(TimelineOf, Timeline *MainWindow::*, &MainWindow::m_timeline)
ACCESS(PlayerOf, VideoPlayer *MainWindow::*, &MainWindow::m_player)
ACCESS(CreateNull, bool (MainWindow::*)(int), &MainWindow::createNullAtSolvePoint)
#undef ACCESS

void clickAt(GLPreview *preview, const QPointF& pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, pos, preview->mapToGlobal(pos), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(preview, &press);
}

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

    // The overlay gates use the first solve: t1 is its reference pose, t2 its last pose.
    (fresh.*member(Apply{}))(firstEmitted);
    Timeline *timeline = fresh.*member(TimelineOf{});
    VideoPlayer *player = fresh.*member(PlayerOf{});
    const QSize canvas = timeline->projectOutputSize();
    const double t1 = 5.0, t2 = 5.0 + 9.0/24.0;
    const QVector<QVector3D> cloudPoints = freshCamera.solvePoints();

    // G4: each point is projectWorldPoint(w(p), projectCameraAt(t)); one behind the pinhole is left out.
    auto withBehind = cloudPoints;
    withBehind.append(QVector3D(0, 0, -3));
    freshCamera.setSolvePoints(withBehind);
    (fresh.*member(Sync{}))();
    const auto worldPoints = freshCamera.solvePointsWorldPx();
    const auto projectedAt = [&](double sec) {
        return camera3d::projectedSolvePoints(freshCamera, sec, canvas);
    };
    const auto matchesProjection = [&](double sec, const QVector<QPointF>& got) {
        const Camera3DState state = timeline->projectCameraAt(sec);
        QVector<QPointF> expected;
        for (const QVector3D& w : worldPoints)
            if (const auto px = camera3d::projectWorldPoint(w, state, canvas)) expected.append(*px);
        bool same = !expected.isEmpty() && got.size() == expected.size();
        for (qsizetype i = 0; same && i < got.size(); ++i)
            same = QLineF(got[i], expected[i]).length() < 1e-6;
        return same;
    };
    const auto atT1 = projectedAt(t1), atT2 = projectedAt(t2);
    const bool g4Match = matchesProjection(t1, atT1) && matchesProjection(t2, atT2);
    const Camera3DState stateT1 = timeline->projectCameraAt(t1);
    const bool behindOut = worldPoints.size() == withBehind.size()
        && !camera3d::projectWorldPoint(worldPoints.last(), stateT1, canvas)
        && atT1.size() == cloudPoints.size();
    // The animated camera is read at the asked time, not at its static state.
    double moved = 0;
    for (qsizetype i = 0; i < std::min(atT1.size(), atT2.size()); ++i)
        moved = std::max(moved, QLineF(atT1[i], atT2[i]).length());
    std::fprintf(stderr, "G4 cloud=%lld withBehind=%lld atT1=%lld atT2=%lld match=%d behindOut=%d moved=%.3f\n",
        static_cast<long long>(cloudPoints.size()), static_cast<long long>(withBehind.size()),
        static_cast<long long>(atT1.size()), static_cast<long long>(atT2.size()), g4Match, behindOut, moved);
    tally.gate(4, g4Match && behindOut && moved > 1.0);

    // G5: same CV (x, y) at z = 2 and 4 from the reference pose; the farther lands nearer the centre.
    // Hand-written pinhole: canvas centre + F*(x/z, y/z) (analysis size == mapping canvas size).
    Camera3D depthPair = freshCamera;
    const QPointF cv(0.2, 0.1);
    depthPair.setSolvePoints({QVector3D(float(cv.x()), float(cv.y()), 2), QVector3D(float(cv.x()), float(cv.y()), 4)});
    const auto pair = camera3d::projectedSolvePoints(depthPair, t1, canvas);
    const QPointF centre(canvas.width()/2.0, canvas.height()/2.0);
    const double focal = freshCamera.solveMapping() ? freshCamera.solveMapping()->focalPx : 0.0;
    const bool pairOk = pair.size() == 2 && focal > 0;
    const double nearDist = pairOk ? QLineF(centre, pair[0]).length() : 0.0;
    const double farDist = pairOk ? QLineF(centre, pair[1]).length() : 0.0;
    const double nearErr = pairOk ? QLineF(pair[0], centre + cv*focal/2.0).length() : -1.0;
    const double farErr = pairOk ? QLineF(pair[1], centre + cv*focal/4.0).length() : -1.0;
    std::fprintf(stderr, "G5 points=%lld focal=%.3f nearDist=%.3f farDist=%.3f nearErr=%.4f farErr=%.4f\n",
        static_cast<long long>(pair.size()), focal, nearDist, farDist, nearErr, farErr);
    tally.gate(5, pairOk && farDist > 0 && farDist < nearDist && nearErr < 1.0 && farErr < 1.0);

    // G6: the view toggle drives the overlay and it follows the playhead and the camera; the
    // upload frame stays byte-identical. A point in front at t1 and behind at t2 makes the
    // count show which time was projected.
    freshCamera.setSolvePoints(cloudPoints);
    (fresh.*member(Sync{}))();
    GLPreview *preview = player->glPreview();
    QImage frame(64, 36, QImage::Format_RGB32);
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x)
            frame.setPixel(x, y, qRgb(4*x, 7*y, x+y));
    preview->displayFrame(frame);
    const QImage uploadOff = preview->uploadFrameForTest();
    timeline->setPlayheadPosition(t1);
    auto *toggle = fresh.findChild<QAction *>(QStringLiteral("solvePointOverlayAction"));
    const bool toggleOk = toggle && toggle->isCheckable() && !toggle->isChecked()
        && toggle->text() == QStringLiteral("解析点群を表示");
    const int countDefault = preview->solvePointOverlayCountForTest();
    if (toggleOk) toggle->setChecked(true);
    const int countOn = preview->solvePointOverlayCountForTest();
    const QImage uploadOn = preview->uploadFrameForTest();
    const bool uploadSame = !uploadOff.isNull() && uploadOff.format() == uploadOn.format()
        && uploadOff.size() == uploadOn.size() && uploadOff.sizeInBytes() == uploadOn.sizeInBytes()
        && std::memcmp(uploadOff.constBits(), uploadOn.constBits(), size_t(uploadOff.sizeInBytes())) == 0
        && uploadOff.pixel(10, 10) != uploadOff.pixel(50, 30);

    QVector3D frontThenBehind;
    bool flipFound = false;
    for (int x = -400; !flipFound && x <= 400; ++x) {
        Camera3D probe = freshCamera;
        const QVector3D candidate(x*0.05f, 0, 0.5f);
        probe.setSolvePoints({candidate});
        flipFound = camera3d::projectedSolvePoints(probe, t1, canvas).size() == 1
            && camera3d::projectedSolvePoints(probe, t2, canvas).isEmpty();
        if (flipFound) frontThenBehind = candidate;
    }
    auto withFlip = cloudPoints;
    withFlip.append(frontThenBehind);
    freshCamera.setSolvePoints(withFlip);
    (fresh.*member(Sync{}))();
    const int atSyncT1 = preview->solvePointOverlayCountForTest();
    const int expectT1 = int(projectedAt(t1).size()), expectT2 = int(projectedAt(t2).size());
    emit timeline->positionChanged(t2);
    const int afterSeek = preview->solvePointOverlayCountForTest();
    emit timeline->scrubPositionChanged(t1);
    const int afterScrub = preview->solvePointOverlayCountForTest();
    emit player->positionChanged(t2);
    const int afterPlayer = preview->solvePointOverlayCountForTest();
    const bool follows = flipFound && expectT1 == expectT2 + 1 && atSyncT1 == expectT1
        && afterSeek == expectT2 && afterScrub == expectT1 && afterPlayer == expectT2;

    // A planar solve clears the cloud and its undo brings it back, both through the camera sync.
    timeline->setPlayheadPosition(t1);
    (fresh.*member(Apply{}))(planar);
    const int afterPlanar = preview->solvePointOverlayCountForTest();
    (fresh.*member(Undo{}))();
    const int afterUndo = preview->solvePointOverlayCountForTest();
    if (toggleOk) toggle->setChecked(false);
    const int countOff = preview->solvePointOverlayCountForTest();
    std::fprintf(stderr, "G6 toggle=%d default=%d on=%d solvePoints=%lld off=%d uploadSame=%d flip=%d sync=%d seek=%d scrub=%d player=%d expectT1=%d expectT2=%d planar=%d undo=%d\n",
        toggleOk, countDefault, countOn, static_cast<long long>(cloudPoints.size()), countOff, uploadSame,
        flipFound, atSyncT1, afterSeek, afterScrub, afterPlayer, expectT1, expectT2, afterPlanar, afterUndo);
    const bool overlayLogic = toggleOk && countDefault == 0 && !cloudPoints.isEmpty()
        && countOn == int(cloudPoints.size()) && countOff == 0 && uploadSame && follows
        && afterPlanar == 0 && afterUndo == expectT1;

    // G7: a real click on a cross creates a hidden 3D null at that point. A point behind the
    // pinhole sits first in the cloud, so the cross index and the cloud index differ by one.
    timeline->setPlayheadPosition(t1);
    auto cloudWithBehind = cloudPoints;
    cloudWithBehind.prepend(QVector3D(0, 0, -3));
    freshCamera.setSolvePoints(cloudWithBehind);
    (fresh.*member(Sync{}))();
    if (toggleOk) toggle->setChecked(true);
    preview->resize(1280, 720);
    const auto nullWorld = freshCamera.solvePointsWorldPx();
    const QVector<QPointF> crosses = projectedAt(t1);
    const QRectF box = preview->letterboxRect();
    const auto toWidget = [&](const QPointF& p) {
        return QPointF(box.left() + p.x()*box.width()/canvas.width(),
                       box.top() + p.y()*box.height()/canvas.height());
    };
    // Pick a cross with no other cross near it, past the first so the index offset shows.
    qsizetype pick = -1;
    for (qsizetype i = 1; pick < 0 && i < crosses.size(); ++i) {
        bool alone = true;
        for (qsizetype j = 0; alone && j < crosses.size(); ++j)
            alone = i == j || QLineF(toWidget(crosses[i]), toWidget(crosses[j])).length() > 10;
        if (alone) pick = i;
    }
    const int tracksBefore = int(timeline->videoTracks().size());
    const int undoBefore = timeline->undoManager()->currentIndex();
    if (pick >= 0) clickAt(preview, toWidget(crosses[pick]));
    const int tracksAfter = int(timeline->videoTracks().size());
    const int undoAfter = timeline->undoManager()->currentIndex();
    const bool oneNull = tracksAfter == tracksBefore + 1 && undoAfter == undoBefore + 1
        && timeline->videoTracks().last()->clips().size() == 1;
    double fieldErr = -1, maxProjErr = -1, startErr = -1, durationErr = -1;
    bool hidden = false, diagonalsCross = false;
    if (oneNull) {
        const ClipInfo null = timeline->videoTracks().last()->clips().first();
        const QVector3D w = nullWorld[pick + 1];
        fieldErr = std::max({std::abs(null.layer3D.worldX - w.x()), std::abs(null.layer3D.worldY - w.y()),
                             std::abs(null.layer3D.positionZ - w.z())});
        hidden = null.is3DLayer && null.opacity == 0.0 && !null.visible
            && null.videoDx == 0.0 && null.videoDy == 0.0;
        startErr = std::abs(null.leadInSec - 5.0);
        durationErr = std::abs(null.effectiveDuration() - 10.0/24.0);
        diagonalsCross = true;
        maxProjErr = 0;
        for (int i = 0; i < 10; ++i) {
            const Camera3DState state = timeline->projectCameraAt(5.0 + i/24.0);
            const QPolygonF quad = Camera3D::projectLayerQuad(null.layer3D, state, canvas);
            const auto expected = camera3d::projectWorldPoint(w, state, canvas);
            QPointF crossing;
            if (quad.size() != 4 || !expected
                || QLineF(quad[0], quad[2]).intersects(QLineF(quad[1], quad[3]), &crossing)
                    != QLineF::BoundedIntersection) {
                diagonalsCross = false;
                continue;
            }
            maxProjErr = std::max(maxProjErr, QLineF(crossing, *expected).length());
        }
    }
    (fresh.*member(Undo{}))();
    const bool undoneNull = int(timeline->videoTracks().size()) == tracksBefore
        && timeline->undoManager()->currentIndex() == undoBefore;
    // The range is the solve's even when the keys say otherwise: 100 frames at 24 fps from 7 s,
    // thinned to every third frame, invalid at both ends, over a hand-set key at 1 s.
    const QJsonObject beforeThinned = freshCamera.toJson();
    auto thinned = firstEmitted;
    thinned.startSec = 7.0;
    thinned.poses = QVector<camsolve::Pose>(100);
    for (int i = 3; i < 99; i += 3)
        thinned.poses[i] = firstEmitted.poses[(i/3) % firstEmitted.poses.size()];
    freshCamera.setCameraKeyframe(1.0, freshCamera.camera());
    (fresh.*member(Apply{}))(thinned);
    const auto nullRangeIs = [&](double start, double duration) {
        if (!(fresh.*member(CreateNull{}))(0)) return false;
        const ClipInfo null = timeline->videoTracks().last()->clips().first();
        std::fprintf(stderr, "G7 range start=%.6f duration=%.6f want %.6f %.6f\n",
            null.leadInSec, null.effectiveDuration(), start, duration);
        const bool same = std::abs(null.leadInSec - start) < 1e-6
            && std::abs(null.effectiveDuration() - duration) < 1e-6;
        (fresh.*member(Undo{}))();
        return same;
    };
    const bool rangeSolved = nullRangeIs(7.0, 100.0/24.0);
    const QJsonObject thinnedJson = freshCamera.toJson();
    freshCamera.fromJson(thinnedJson);
    const bool rangeReloaded = freshCamera.toJson() == thinnedJson && nullRangeIs(7.0, 100.0/24.0);
    (fresh.*member(Apply{}))(firstEmitted);
    (fresh.*member(Undo{}))();
    const bool rangeUndone = nullRangeIs(7.0, 100.0/24.0);
    // An older file has no range: the keys decide, from the 1 s key to the last pose at 11 s
    // plus the smallest key gap, which is the 1/24 s of the earlier solve.
    QJsonObject olderJson = thinnedJson;
    QJsonObject olderMapping = olderJson[QStringLiteral("solveMapping")].toObject();
    const bool rangeStored = olderMapping[QStringLiteral("sampleCount")].toInt() == 100
        && olderMapping[QStringLiteral("startSec")].toDouble() == 7.0
        && olderMapping[QStringLiteral("fps")].toDouble() == 24.0;
    for (const char *key : {"startSec", "sampleCount", "fps"})
        olderMapping.remove(QLatin1String(key));
    olderJson[QStringLiteral("solveMapping")] = olderMapping;
    freshCamera.fromJson(olderJson);
    const bool rangeOlder = nullRangeIs(1.0, 10.0 + 1.0/24.0)
        && freshCamera.toJson() == olderJson;
    freshCamera.fromJson(beforeThinned);
    (fresh.*member(Sync{}))();
    std::fprintf(stderr, "G7 pick=%lld crosses=%lld oneNull=%d fieldErr=%.6f hidden=%d diagonals=%d maxProjErr=%.4f startErr=%.6f durationErr=%.6f undone=%d rangeSolved=%d rangeReloaded=%d rangeUndone=%d rangeStored=%d rangeOlder=%d\n",
        static_cast<long long>(pick), static_cast<long long>(crosses.size()), oneNull, fieldErr, hidden,
        diagonalsCross, maxProjErr, startErr, durationErr, undoneNull,
        rangeSolved, rangeReloaded, rangeUndone, rangeStored, rangeOlder);
    tally.gate(7, oneNull && fieldErr >= 0 && fieldErr < 1e-3 && hidden && diagonalsCross
               && maxProjErr <= 0.5 && startErr < 1e-6 && durationErr < 1e-6 && undoneNull
               && rangeSolved && rangeReloaded && rangeUndone && rangeStored && rangeOlder);

    // G8 range: one axis at a time past 10000 is refused with a status; just inside is created.
    // S and F come from the mapping, so these solver points land on the wanted world values.
    const double nullS = freshCamera.solveMapping()->unitScalePx;
    const double nullF = freshCamera.solveMapping()->focalPx;
    freshCamera.setSolvePoints({QVector3D(float(10001/nullS), 0, 0), QVector3D(0, float(-10001/nullS), 0),
                                QVector3D(0, 0, float((10001 + nullF)/nullS)),
                                QVector3D(float(9999/nullS), 0, 0)});
    (fresh.*member(Sync{}))();
    const QString refusedStatus = QStringLiteral("解析点の座標が範囲外（±10000）のため、ヌルオブジェクトを作成できませんでした");
    bool refused = true;
    for (int i = 0; i < 3; ++i) {
        fresh.statusBar()->clearMessage();
        refused = refused && !(fresh.*member(CreateNull{}))(i)
            && fresh.statusBar()->currentMessage() == refusedStatus;
    }
    const bool noneMade = int(timeline->videoTracks().size()) == tracksBefore
        && timeline->undoManager()->currentIndex() == undoBefore;
    const bool insideMade = (fresh.*member(CreateNull{}))(3)
        && int(timeline->videoTracks().size()) == tracksBefore + 1
        && fresh.statusBar()->currentMessage() != refusedStatus;
    (fresh.*member(Undo{}))();

    // G8 distance: a 2:1 image in a 320x240 widget is 320x160 at y = 40, so canvas (150, 75) of
    // 200x100 is widget (240, 160) at 1.6 widget px per canvas px. 8 widget px is 5 canvas px:
    // 9 widget px away is refused even though it is under 8 canvas px, and the letterbox bar
    // offset must be applied for the first click to hit at all.
    GLPreview clickView;
    clickView.resize(320, 240);
    clickView.setDisplayAspectRatio(2.0);
    clickView.setSolvePointOverlay({QPointF(150, 75), QPointF(20, 20)}, QSize(200, 100), {5, 9});
    QVector<int> clicked;
    QObject::connect(&clickView, &GLPreview::solvePointClicked, &clickView,
        [&](int index) { clicked.append(index); });
    clickAt(&clickView, QPointF(240, 160));   // on the cross
    clickAt(&clickView, QPointF(248, 160));   // exactly 8 px right
    clickAt(&clickView, QPointF(249, 160));   // 9 px right
    clickAt(&clickView, QPointF(240, 169));   // 9 px down
    clickAt(&clickView, QPointF(32, 72));     // on the second cross: (20*1.6, 20*1.6 + 40)
    const bool distanceOk = clicked == QVector<int>{5, 5, 9};
    // The nearer of two crosses inside the radius wins, and an off overlay clicks nothing.
    clickView.setSolvePointOverlay({QPointF(150, 75), QPointF(155, 75)}, QSize(200, 100));
    clicked.clear();
    clickAt(&clickView, QPointF(246, 160));   // 6 px from the first, 2 from the second
    const bool nearestOk = clicked == QVector<int>{1};
    // Canvas (150, -10) maps to widget (240, 24) on the bar, past the 4 px arm: it is not drawn,
    // so a click exactly on it picks nothing. (150, -2) at y = 36.8 still shows its lower arm.
    clickView.setSolvePointOverlay({QPointF(150, -10)}, QSize(200, 100));
    clicked.clear();
    clickAt(&clickView, QPointF(240, 24));
    const bool offBoxOk = clicked.isEmpty();
    clickView.setSolvePointOverlay({QPointF(150, -2)}, QSize(200, 100));
    clickAt(&clickView, QPointF(240, 36.8));
    const bool armInBoxOk = clicked == QVector<int>{0};
    clickView.setSolvePointOverlay({}, {});
    clicked.clear();
    clickAt(&clickView, QPointF(240, 160));
    const bool offOk = clicked.isEmpty();
    if (toggleOk) toggle->setChecked(false);
    std::fprintf(stderr, "G8 refused=%d noneMade=%d insideMade=%d distance=%d nearest=%d offBox=%d armInBox=%d off=%d\n",
        refused, noneMade, insideMade, distanceOk, nearestOk, offBoxOk, armInBoxOk, offOk);
    tally.gate(8, refused && noneMade && insideMade && distanceOk && nearestOk && offBoxOk && armInBoxOk && offOk);

    // The crosses reach the screen at the letterboxed position: a 2:1 image in a 320x240 widget
    // is 320x160 at y = 40, so canvas (150, 75) of 200x100 is widget (240, 160). A point above
    // the canvas lands on the bar and is clipped.
    GLPreview view;
    view.setFixedSize(320, 240);
    view.setDisplayAspectRatio(2.0);
    view.displayFrame(frame);
    view.show();
    bool glOk = false;
    QImage plainGrab;
    for (int i = 0; i < 8 && !glOk; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        plainGrab = view.grabFramebuffer();
        glOk = view.context() && view.context()->isValid() && !plainGrab.isNull();
    }
    if (!glOk) {
        if (overlayLogic) tally.skip(6, "no usable QOpenGLWidget context");
        else tally.gate(6, false);
        return tally.finish();
    }
    view.setSolvePointOverlay({QPointF(150, 75), QPointF(150, -10)}, QSize(200, 100));
    const QImage crossGrab = view.grabFramebuffer().convertToFormat(plainGrab.format());
    const double dpr = plainGrab.width() / 320.0;
    QRect changed;
    for (int y = 0; y < crossGrab.height(); ++y)
        for (int x = 0; x < crossGrab.width(); ++x)
            if (crossGrab.pixel(x, y) != plainGrab.pixel(x, y)) changed |= QRect(x, y, 1, 1);
    const bool painted = !changed.isEmpty()
        && std::abs(changed.center().x() - 240*dpr) <= 2*dpr && std::abs(changed.center().y() - 160*dpr) <= 2*dpr
        && changed.width() <= 12*dpr && changed.height() <= 12*dpr;
    std::fprintf(stderr, "G6 changed=(%d,%d %dx%d) dpr=%.2f painted=%d\n",
        changed.x(), changed.y(), changed.width(), changed.height(), dpr, painted);
    tally.gate(6, overlayLogic && painted);
    return tally.finish();
}
