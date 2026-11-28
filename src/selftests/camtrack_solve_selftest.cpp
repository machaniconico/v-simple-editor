#include "../BundleAdjustment.h"
#include "../PlanarTrackerDialog.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QByteArray>
#include <QLabel>
#include <QRegularExpression>
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
ACCESS(Apply, void (MainWindow::*)(const camsolve::CameraSolveApplied&), &MainWindow::applyCameraSolve)
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
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
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
    camsolve::CameraSolveApplied emittedApplied;
    QObject::connect(&dialog, &PlanarTrackerDialog::cameraSolveApplied, &window,
        [&](const camsolve::CameraSolveApplied& applied) {
            ++appliedSignals; emittedApplied = applied; emitted = applied.poses;
            emittedFps = applied.fps; emittedStart = applied.startSec;
            (window.*member(Apply{}))(applied);
        });
    // The feature path applies the reference-centred, scaled mapping of the emitted solve.
    const auto expectedState = [&](qsizetype i) {
        auto poses = emittedApplied.poses;
        auto points = emittedApplied.points;
        camsolve::normaliseToReference(poses, points, camsolve::referencePoseIndex(poses));
        return camsolve::poseToProjectCameraState(poses[i], camera.camera(), emittedApplied.mapping);
    };
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
        const auto expected = expectedState(i);
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

    // Scenario 0 reaches bundle adjustment and is rejected (RMS > 2 px); scenario 1 is accepted.
    const auto perturbed = [&](int scenario) {
        auto input = tracks;
        std::mt19937 noiseRng(623);
        const auto uniform = [&] { return (double(noiseRng())+.5)/4294967296.0; };
        for (int point = 0; point < input.tracks.size(); ++point) {
            for (auto& obs : input.tracks[point].observations) {
                if (scenario == 0) {
                    // Keep endpoint triangulation clean; minority interior outliers
                    // leave enough inliers for every two-view initialization.
                    if (point % 8 == 0 && obs.first > 0 && obs.first < input.frameCount-1)
                        obs.second += QPointF(obs.first % 2 ? 20 : -20, point % 3 ? -20 : 20);
                } else {
                    const double radius = .5*std::sqrt(-2*std::log(uniform()));
                    const double angle = 2*pi*uniform();
                    obs.second += QPointF(radius*std::cos(angle), radius*std::sin(angle));
                }
            }
        }
        return input;
    };
    const auto after = camera.toJson();
    auto stationary = tracks;
    for (auto& track : stationary.tracks)
        for (auto& obs : track.observations) obs.second = track.observations.first().second;
    dialog.setFeatureTrackingResult(stationary, {1280,720}, 0);
    const bool rejected = !dialog.solveFeatureCamera();
    const QString failure = dialog.findChild<QLabel*>("featureSolveSummary")->text();
    const double stationaryRms = dialog.featureSolveResult().bundle.rms;
    const bool unchanged = camera.toJson() == after;
    // The stationary scene fails before bundle adjustment, so no RMS exists. A scene
    // with interior outliers fails after it; its summary must carry the actual number,
    // not just the fixed label. The planar-scene hint must be specific to failures.
    dialog.setFeatureTrackingResult(perturbed(0), {1280,720}, 0);
    const bool outlierRejected = !dialog.solveFeatureCamera();
    const QString outlierFailure = dialog.findChild<QLabel*>("featureSolveSummary")->text();
    const double outlierRms = dialog.featureSolveResult().bundle.rms;
    const bool outlierUnchanged = camera.toJson() == after;
    const auto rmsMatch = QRegularExpression(QStringLiteral("RMS 再投影誤差: (\\S+) px")).match(outlierFailure);
    bool numeric = false;
    const double shownRms = rmsMatch.hasMatch() ? rmsMatch.captured(1).toDouble(&numeric) : 0.0;
    const bool rmsShown = numeric && std::isfinite(outlierRms) && std::abs(shownRms-outlierRms) <= 1e-3;
    const bool infinityShown = !std::isfinite(stationaryRms)
        && failure.contains(QStringLiteral("RMS 再投影誤差: ∞（算出不可） px"));
    const bool hintOnFailure = failure.contains(QStringLiteral("平面シーン"))
        && outlierFailure.contains(QStringLiteral("平面シーン"));
    const bool noHintOnSuccess = !successSummary.contains(QStringLiteral("平面シーン"));
    std::fprintf(stderr, "G3 rejected=%d unchanged=%d stationaryRms=%g infinityShown=%d message=%s\n",
        rejected, unchanged, stationaryRms, infinityShown, failure.toUtf8().replace('\n', " | ").constData());
    std::fprintf(stderr, "G3 outlierRejected=%d outlierUnchanged=%d appliedSignals=%d numeric=%d shownRms=%.9g bundleRms=%.9g rmsShown=%d hintOnFailure=%d noHintOnSuccess=%d message=%s\n",
        outlierRejected, outlierUnchanged, appliedSignals, numeric, shownRms, outlierRms, rmsShown,
        hintOnFailure, noHintOnSuccess, outlierFailure.toUtf8().replace('\n', " | ").constData());
    gate(3, rejected && unchanged && infinityShown && outlierRejected && outlierUnchanged
         && appliedSignals == 1 && rmsShown && hintOnFailure && noHintOnSuccess);

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
            const auto expectedCamera = expectedState(i);
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
    // Exercise the BA acceptance boundary through the real dialog/apply path.
    // The environment switch selects only the preserved dense test oracle.
    const QByteArray oldBackend = qgetenv("VEDITOR_BUNDLE_DENSE_SELFTEST");
    const bool hadBackend = qEnvironmentVariableIsSet("VEDITOR_BUNDLE_DENSE_SELFTEST");
    bool decided = true, parityAll = true;
    for (int scenario = 0; scenario < 2; ++scenario) {
        const auto input = perturbed(scenario);
        sfm::BundleResult oracle;
        QString oracleMessage;
        for (bool dense : {true, false}) {
            qputenv("VEDITOR_BUNDLE_DENSE_SELFTEST", dense ? "1" : "0");
            dialog.setFeatureClipTiming(0, 10, 1, false, false);
            dialog.setFeatureTrackingResult(input, {1280,720}, 0);
            const auto priorCamera = camera.toJson();
            const int priorSignals = appliedSignals;
            const bool accepted = dialog.solveFeatureCamera();
            const auto actual = dialog.featureSolveResult().bundle;
            const QString message = dialog.findChild<QLabel*>("featureSolveSummary")->text();
            const bool reached = std::isfinite(actual.rms) && !actual.rmsHistory.isEmpty();
            const bool unchanged = camera.toJson() == priorCamera;
            const bool warned = message.contains(QStringLiteral("許容値 2 px"));
            const auto* positionKeys = camera.track(Camera3DProperty::PositionX);
            bool appliedNow = appliedSignals == priorSignals+1
                && positionKeys && positionKeys->count() >= input.frameCount
                && emitted.size() == input.frameCount;
            for (int i = 0; appliedNow && i < emitted.size(); ++i) {
                const auto expected = expectedState(i);
                const auto keyed = camera.getCameraAt(emittedStart+double(i)/emittedFps);
                appliedNow = (keyed.position-expected.position).length() < 1e-5
                    && (keyed.target-expected.target).length() < 1e-5;
            }
            const bool decision = scenario == 0
                ? (!accepted && !actual.valid && actual.rms > 2 && warned && unchanged
                   && appliedSignals == priorSignals)
                : (accepted && actual.valid && actual.converged && actual.rms < 2 && appliedNow);
            // Identical paths would agree trivially; require that distinct backends ran.
            const auto expectedBackend = dense ? sfm::BundleResult::Backend::Dense
                                               : sfm::BundleResult::Backend::Schur;
            const bool backend = actual.backend == expectedBackend
                && (dense || oracle.backend != actual.backend);
            const bool parity = backend && (dense || (actual.valid == oracle.valid
                && actual.converged == oracle.converged && actual.reason == oracle.reason
                && warned == oracleMessage.contains(QStringLiteral("許容値 2 px"))));
            // Both sides of every parity comparison; oracle* are the preceding dense run's values.
            const bool oracleWarned = oracleMessage.contains(QStringLiteral("許容値 2 px"));
            std::fprintf(stderr, "G6/G7 scenario=%d dense=%d reached=%d accepted=%d valid=%d oracleValid=%d converged=%d oracleConverged=%d rms=%g warned=%d oracleWarned=%d unchanged=%d applied=%d decision=%d backend=%d oracleBackend=%d backendOk=%d parity=%d reason=%s oracleReason=%s\n",
                scenario, dense, reached, accepted, actual.valid, oracle.valid, actual.converged,
                oracle.converged, actual.rms, warned, oracleWarned, unchanged, appliedNow, decision,
                int(actual.backend), int(oracle.backend), backend, parity,
                actual.reason.toUtf8().constData(), oracle.reason.toUtf8().constData());
            decided = decided && reached && decision;
            parityAll = parityAll && parity;
            if (dense) { oracle = actual; oracleMessage = message; }
        }
    }
    gate(6, decided);
    gate(7, parityAll);
    if (hadBackend) qputenv("VEDITOR_BUNDLE_DENSE_SELFTEST", oldBackend);
    else qunsetenv("VEDITOR_BUNDLE_DENSE_SELFTEST");
    return tally.finish();
}
