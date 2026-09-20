#include "../BundleAdjustment.h"
#include "../PlanarTrackerDialog.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"

#include <QLabel>
#include <QSpinBox>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace {
// Access the existing production apply/undo path without expanding MainWindow's API.
template<class Tag, typename Tag::Type Member> struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
#define ACCESS(Name, TypeExpr, MemberExpr) \
    struct Name { using Type = TypeExpr; friend Type member(Name); }; \
    template struct Access<Name, MemberExpr>;
ACCESS(Apply, void (MainWindow::*)(const QVector<camsolve::Pose>&, double, double), &MainWindow::applyCameraSolve)
ACCESS(Undo, void (MainWindow::*)(), &MainWindow::undoAction)
ACCESS(Camera, Camera3D MainWindow::*, &MainWindow::m_projectCamera)
ACCESS(TimelineMember, Timeline* MainWindow::*, &MainWindow::m_timeline)
ACCESS(UndoSlot, QJsonObject MainWindow::*, &MainWindow::m_projectCameraUndoSlot)
#undef ACCESS
constexpr double pi = 3.14159265358979323846;

// Noise-free, nonplanar 64-point / 10-frame scene. Nonuniform camera arc,
// roughly 0.26-unit first baseline and depths 4..8 give substantial parallax.
sfm::MultiViewTracks scene(QVector<camsolve::Pose>& truth)
{
    sfm::MultiViewTracks tracks;
    tracks.frameCount = 10;
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

int runCamtrackSolveSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QVector<camsolve::Pose> truth;
    const auto tracks = scene(truth);
    MainWindow window;
    auto& camera = window.*member(Camera{});
    auto* timeline = window.*member(TimelineMember{});
    const auto before = camera.toJson();
    const int depth = timeline->undoManager()->currentIndex();
    const auto serial = timeline->undoManager()->saveSerial();
    PlanarTrackerDialog dialog;
    dialog.setCameraSolveContext({1280,720}, 800, 24, 5);
    dialog.setFeatureTrackingSource("synthetic", 24, 240);
    dialog.setFeatureTrackingResult(tracks, {1280,720}, 0);
    int appliedSignals = 0;
    double emittedFps = 0, emittedStart = 0;
    QVector<camsolve::Pose> emitted;
    QObject::connect(&dialog, &PlanarTrackerDialog::cameraSolveApplied, &window,
        [&](const QVector<camsolve::Pose>& poses, double fps, double start) {
            ++appliedSignals; emitted = poses; emittedFps = fps; emittedStart = start;
            (window.*member(Apply{}))(poses, fps, start);
        });
    const bool solved = dialog.solveFeatureCamera();
    const auto result = dialog.featureSolveResult();
    double rotation = 0, direction = 0;
    bool accuracy = result.bundle.poses.size() == truth.size();
    for (int i = 0; i < result.bundle.poses.size() && i < truth.size(); ++i) {
        const auto& p = result.bundle.poses[i];
        double trace = 0;
        for (int j = 0; j < 9; ++j) trace += p.R[j]*truth[i].R[j];
        rotation = std::max(rotation, std::acos(std::clamp((trace-1)/2, -1.0, 1.0))*180/pi);
        if (i > 0) {
            const double dot = QVector3D::dotProduct(p.t.normalized(), truth[i].t.normalized());
            direction = std::max(direction, std::acos(std::clamp(dot, -1.0, 1.0))*180/pi);
        }
        accuracy = accuracy && p.valid;
    }
    const auto* keys = camera.track(Camera3DProperty::PositionX);
    bool applied = keys && keys->count() == tracks.frameCount && camera.camera().trueProjection;
    for (int i = 0; applied && i < emitted.size(); ++i) {
        const auto expected = camsolve::poseToCameraState(emitted[i], camera.camera());
        const auto actual = camera.getCameraAt(5+double(i)/24);
        applied = (actual.position-expected.position).length() < 1e-5
            && (actual.target-expected.target).length() < 1e-5;
    }
    const QString successSummary = dialog.findChild<QLabel*>("featureSolveSummary")->text();
    const bool reportedCounts = successSummary.contains(QStringLiteral("使用フレーム数: 10 / 10"))
        && successSummary.contains(QStringLiteral("使用点数: 64 / 64")) && successSummary.contains("RMS");
    std::fprintf(stderr, "G1 accuracy=%d reportedCounts=%d summary=%s\n",
        accuracy, reportedCounts, qPrintable(successSummary));
    std::fprintf(stderr, "G1 solved=%d appliedSignals=%d poses=%lld rotation=%g direction=%g rms=%g applied=%d reason=%s\n",
        solved, appliedSignals, static_cast<long long>(result.bundle.poses.size()), rotation, direction,
        result.bundle.rms, applied, qPrintable(result.bundle.reason));
    gate(1, solved && appliedSignals == 1 && accuracy && rotation < 1 && direction < 3 && applied && reportedCounts);

    bool boundaries = true;
    for (int extraFrames : {0,1}) for (int extraPoints : {0,1}) {
        sfm::MultiViewTracks input;
        input.frameCount = sfm::kMaxBundleFrames + extraFrames;
        for (int i = 0; i < sfm::kMaxBundlePoints + extraPoints; ++i) {
            sfm::Track2D track;
            track.pointId = i; track.strength = i;
            for (int f = 0; f < input.frameCount; ++f)
                track.observations.append(qMakePair(f, QPointF(i,f)));
            input.tracks.append(track);
        }
        const auto sampled = sfm::sampleCameraTracks(input);
        bool mapping = sampled.sourceFrames.first() == 0 && sampled.sourceFrames.last() == input.frameCount-1;
        for (int f = 0; f < sampled.sourceFrames.size(); ++f)
            mapping = mapping && sampled.input.tracks.first().observations[f].second.y() == sampled.sourceFrames[f];
        const bool strongest = sampled.input.tracks.first().pointId == input.tracks.size()-1
            && sampled.input.tracks.last().pointId == extraPoints;
        const bool ok = sampled.input.frameCount == sfm::kMaxBundleFrames
            && sampled.input.tracks.size() == sfm::kMaxBundlePoints
            && sampled.thinned == bool(extraFrames || extraPoints) && mapping && strongest;
        std::fprintf(stderr, "G2 extraFrames=%d extraPoints=%d usedFrames=%d usedPoints=%lld thinned=%d mapping=%d strongest=%d\n",
            extraFrames, extraPoints, sampled.input.frameCount, static_cast<long long>(sampled.input.tracks.size()),
            sampled.thinned, mapping, strongest);
        PlanarTrackerDialog boundaryDialog;
        boundaryDialog.setFeatureTrackingSource("synthetic", 24, input.frameCount);
        boundaryDialog.setFeatureTrackingResult(input, {1280,720}, 0);
        // Force an out-of-range failure after production sampling, before numerical solve.
        boundaryDialog.setFeatureClipTiming(1, 2, 1, false);
        const bool blocked = !boundaryDialog.solveFeatureCamera();
        const QString report = boundaryDialog.findChild<QLabel*>("featureSolveSummary")->text();
        const bool reported = report.contains(QStringLiteral("使用フレーム数: %1 / %2")
            .arg(sfm::kMaxBundleFrames).arg(input.frameCount))
            && report.contains(QStringLiteral("使用点数: %1 / %2")
                .arg(sfm::kMaxBundlePoints).arg(input.tracks.size()))
            && report.contains(QStringLiteral("間引きあり")) == bool(extraFrames || extraPoints);
        std::fprintf(stderr, "G2 blocked=%d reported=%d report=%s\n", blocked, reported, qPrintable(report));
        boundaries = boundaries && ok && blocked && reported;
    }
    gate(2, boundaries);

    const auto after = camera.toJson();
    auto stationary = tracks;
    for (auto& track : stationary.tracks)
        for (auto& obs : track.observations) obs.second = track.observations.first().second;
    dialog.setFeatureTrackingResult(stationary, {1280,720}, 0);
    const bool rejected = !dialog.solveFeatureCamera();
    const QString failure = dialog.findChild<QLabel*>("featureSolveSummary")->text();
    const bool unchanged = camera.toJson() == after;
    const bool reported = failure.contains("RMS") && failure.contains(QStringLiteral("平面シーン"));
    std::fprintf(stderr, "G3 rejected=%d appliedSignals=%d unchanged=%d reported=%d message=%s\n",
        rejected, appliedSignals, unchanged, reported, qPrintable(failure));
    gate(3, rejected && appliedSignals == 1 && unchanged && reported);

    (window.*member(Undo{}))();
    const bool restored = camera.toJson() == before;
    const bool slotEmpty = (window.*member(UndoSlot{})).isEmpty();
    const bool sameDepth = timeline->undoManager()->currentIndex() == depth
        && timeline->undoManager()->saveSerial() == serial;
    std::fprintf(stderr, "G4 changed=%d restored=%d slotEmpty=%d sameDepth=%d\n",
        after != before, restored, slotEmpty, sameDepth);
    gate(4, after != before && restored && slotEmpty && sameDepth);

    dialog.setFeatureClipTiming(2, 4, 2, false);
    const int defaultStart = dialog.findChild<QSpinBox*>("featureStart")->value();
    const int defaultEnd = dialog.findChild<QSpinBox*>("featureEnd")->value();
    bool times = true;
    for (bool reversed : {false,true}) {
        dialog.setFeatureClipTiming(2, 4, 2, reversed);
        dialog.setFeatureTrackingResult(tracks, {1280,720}, 54);
        const bool ok = dialog.solveFeatureCamera();
        const double expectedStart = 5 + (reversed ? 4-63.0/24 : 54.0/24-2)/2;
        const bool timing = ok && emittedFps == 48 && std::abs(emittedStart-expectedStart) < 1e-10;
        bool order = ok && emitted.size() == tracks.frameCount
            && dialog.featureSolveResult().bundle.poses.size() == tracks.frameCount;
        for (int i = 0; order && i < emitted.size(); ++i) {
            const int source = reversed ? tracks.frameCount-1-i : i;
            order = emitted[i].t == dialog.featureSolveResult().bundle.poses[source].t;
            const double expectedTime = 5 + (reversed ? 4-(54.0+source)/24 : (54.0+source)/24-2)/2;
            const auto expectedCamera = camsolve::poseToCameraState(emitted[i], camera.camera());
            const auto actualCamera = camera.getCameraAt(expectedTime);
            order = order && std::abs(emittedStart+i/emittedFps-expectedTime) < 1e-10
                && (actualCamera.position-expectedCamera.position).length() < 1e-5
                && (actualCamera.target-expectedCamera.target).length() < 1e-5;
        }
        std::fprintf(stderr, "G5 reversed=%d solved=%d fps=%g start=%.17g expected=%.17g order=%d\n",
            reversed, ok, emittedFps, emittedStart, expectedStart, order);
        times = times && timing && order;
    }
    const int previousSignals = appliedSignals;
    const auto beforeOutside = camera.toJson();
    bool outside = true;
    for (int firstOutside : {0, 90}) {
        dialog.setFeatureTrackingResult(tracks, {1280,720}, firstOutside);
        const bool denied = !dialog.solveFeatureCamera();
        const bool noSignal = appliedSignals == previousSignals;
        const bool noChange = camera.toJson() == beforeOutside;
        const bool warned = dialog.findChild<QLabel*>("featureSolveSummary")->text()
            .contains(QStringLiteral("使用区間外"));
        std::fprintf(stderr, "G5 outsideFirst=%d denied=%d noSignal=%d noChange=%d warned=%d\n",
            firstOutside, denied, noSignal, noChange, warned);
        outside = outside && denied && noSignal && noChange && warned;
    }
    // A ramped clip has no linear inverse, so the solve must refuse instead of
    // placing keyframes at silently wrong times.
    dialog.setFeatureClipTiming(2, 4, 2, false, true);
    dialog.setFeatureTrackingResult(tracks, {1280,720}, 54);
    const bool rampDenied = !dialog.solveFeatureCamera();
    const bool rampNoSignal = appliedSignals == previousSignals;
    const bool rampNoChange = camera.toJson() == beforeOutside;
    const bool rampWarned = dialog.findChild<QLabel*>("featureSolveSummary")->text()
        .contains(QStringLiteral("速度ランプ"));
    dialog.setFeatureClipTiming(2, 4, 2, false, false);
    const bool linearAccepted = dialog.solveFeatureCamera();
    std::fprintf(stderr, "G5 rampDenied=%d rampNoSignal=%d rampNoChange=%d rampWarned=%d linearAccepted=%d\n",
        rampDenied, rampNoSignal, rampNoChange, rampWarned, linearAccepted);
    const bool ramped = rampDenied && rampNoSignal && rampNoChange && rampWarned && linearAccepted;
    const bool defaults = defaultStart == 48 && defaultEnd == std::min(95, 48+sfm::kMaxBundleFrames-1);
    std::fprintf(stderr, "G5 defaultStart=%d defaultEnd=%d defaults=%d mappedTimes=%d outsideRejected=%d\n",
        defaultStart, defaultEnd, defaults, times, outside);
    gate(5, defaults && times && outside && ramped);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
