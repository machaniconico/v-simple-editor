#include "../PlanarTrackerDialog.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <random>
#include <limits>

namespace {
// Reaches MainWindow's apply path and project camera without widening its API.
template<class Tag, typename Tag::Type Member> struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
#define ACCESS(Name, TypeExpr, MemberExpr) \
    struct Name { using Type = TypeExpr; friend Type member(Name); }; \
    template struct Access<Name, MemberExpr>;
ACCESS(Apply, void (MainWindow::*)(const camsolve::CameraSolveApplied&), &MainWindow::applyCameraSolve)
ACCESS(Camera, Camera3D MainWindow::*, &MainWindow::m_projectCamera)
ACCESS(UndoSlot, QJsonObject MainWindow::*, &MainWindow::m_projectCameraUndoSlot)
#undef ACCESS

// Noise-free, nonplanar 64-point / 10-frame scene (camera arc, depths 4..8 in front of
// frame 0). Frame 0 is the identity pose.
sfm::MultiViewTracks solveScene(QVector<camsolve::Pose>& truth, double focal)
{
    sfm::MultiViewTracks tracks;
    tracks.frameCount = 10;
    truth.clear();
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
                QPointF(focal*projected[0]/projected[2]+640, focal*projected[1]/projected[2]+360)));
        }
        tracks.tracks.append(track);
    }
    return tracks;
}

// f / median(depth in the reference camera) for the emitted solve, from its own first valid pose.
double referenceScale(const camsolve::CameraSolveApplied& applied, double f)
{
    const camsolve::Pose* ref = nullptr;
    for (const auto& pose : applied.poses)
        if (pose.valid) { ref = &pose; break; }
    QVector<double> depths;
    if (ref)
        for (const QVector3D& p : applied.points) {
            const double z = ref->R[6]*p.x() + ref->R[7]*p.y() + ref->R[8]*p.z() + ref->t.z();
            if (z > 0) depths.append(z);
        }
    if (depths.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(depths.begin(), depths.end());
    const qsizetype n = depths.size();
    return f / (n % 2 ? depths[n/2] : (depths[n/2-1] + depths[n/2]) / 2);
}

QImage texture(QSize size)
{
    QImage image(size, QImage::Format_RGB888);
    image.fill(QColor(96, 96, 96));
    QPainter painter(&image);
    std::mt19937 rng(621);
    for (int i = 0; i < 500; ++i) {
        const int x = 30 + int(rng() % (size.width() - 80));
        const int y = 30 + int(rng() % (size.height() - 80));
        const int value = int(rng() % 256);
        // Draw the sizes in a fixed order; argument evaluation order is unspecified.
        const int width = 8 + int(rng() % 16);
        const int height = 8 + int(rng() % 16);
        painter.fillRect(x, y, width, height, QColor(value, value, value));
    }
    return image;
}

bool writeClip(const QString& path, QSize size, bool blank = false, int fps = 10, int frames = 6)
{
    libavcore::EncodeRequest request;
    request.width = size.width(); request.height = size.height();
    request.fps = fps; request.fpsNum = fps; request.fpsDen = 1;
    request.videoBitrateBits = 8000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    const QImage base = texture(size);
    for (int i = 0; i < frames; ++i) {
        QImage frame(size, QImage::Format_RGB888);
        frame.fill(QColor(96, 96, 96));
        if (!blank) {
            QPainter painter(&frame);
            painter.drawImage(2 * i, 0, base);
        }
        if (!encoder.pushFrame(frame, i)) return false;
    }
    return !encoder.finalize().has_value();
}

// Drive the actual UI asynchronously; timer ticks prove the event loop remains live.
// On the timeout (60 s by default), sets timedOut and waits for the worker to
// finish, so the next run is not silently rejected by setFeatureTrackingSource's
// early return. If the worker still runs after stopWaitMs, sets stuck: the
// dialog is unusable and the caller must end the selftest (see abortStuck).
// Once stuck is set, further runs do not touch the dialog.
bool runTracking(PlanarTrackerDialog& dialog, int first, int last, int& ticks, bool& timedOut,
                 bool& stuck, int timeoutMs = 60000, int stopWaitMs = 300000)
{
    if (stuck) return false;
    dialog.findChild<QSpinBox*>("featureStart")->setValue(first);
    dialog.findChild<QSpinBox*>("featureEnd")->setValue(last);
    QEventLoop loop;
    bool finished = false, success = false;
    const auto connection = QObject::connect(&dialog, &PlanarTrackerDialog::featureTrackingFinished,
        &loop, [&](bool ok) { finished = true; success = ok; loop.quit(); });
    QTimer heartbeat, timeout;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++ticks; });
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    heartbeat.start(); timeout.start(timeoutMs);
    dialog.findChild<QPushButton*>("featureTrackButton")->click();
    if (!finished) loop.exec();
    QObject::disconnect(connection);
    if (!finished) {
        timedOut = true;
        const bool stopped = QTest::qWaitFor([&] { return !dialog.featureTrackingRunning(); }, stopWaitMs);
        std::fprintf(stderr, "runTracking %d..%d: timeout after %d ms, worker stopped=%d "
                     "(waited up to %d ms)\n", first, last, timeoutMs, int(stopped), stopWaitMs);
        stuck = !stopped;
    }
    return finished && success;
}

// A worker that did not stop leaves the shared dialog unusable: fail the current
// gate and tell the caller to finish the tally without running later gates.
bool abortStuck(selftest::GateTally& tally, int number)
{
    tally.gate(number, false, "timeout (worker still running; later gates not run)");
    return true;
}

QImage checkerboard()
{
    QImage image(256, 256, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int v = ((x / 8) + (y / 8)) % 2 ? 230 : 25;
            image.setPixelColor(x, y, QColor(v, v, v));
        }
    return image;
}

QString g_capturedWarning;
QtMessageHandler g_previousHandler = nullptr;

void captureWarning(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QStringLiteral("特徴点解析スレッドの停止を待ちきれませんでした")))
        g_capturedWarning = message;
    if (g_previousHandler) g_previousHandler(type, context, message);
}

int expectedFromSummary(const QString& text)
{
    const auto match = QRegularExpression(QStringLiteral("期待 (\\d+) フレーム")).match(text);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

struct PlanarRun {
    bool invoked = false;
    int loads = 0;
    bool buttonEnabled = false;
    bool promptReady = false;
    int applied = 0;
    double fps = 0, start = 0;
    QVector<camsolve::Pose> poses;
    camsolve::CameraSolveApplied solve;
};

PlanarRun runPlanar(const QImage& reference, bool lazy)
{
    PlanarRun result;
    PlanarTrackerDialog dialog;
    dialog.setReferenceFrame(reference);
    dialog.setCameraSolveContext(reference.size(), 320, 10, 2.0);
    const QList<QImage> frames{reference, reference, reference};
    if (lazy)
        dialog.setPlanarFrameLoader([&] { ++result.loads; return frames; });
    else
        dialog.setFrames(frames); // Existing public entry point, still production code.
    const auto connection = QObject::connect(&dialog, &PlanarTrackerDialog::cameraSolveApplied,
        &dialog, [&](const camsolve::CameraSolveApplied& applied) {
            ++result.applied;
            result.solve = applied;
            result.poses = applied.poses; result.fps = applied.fps; result.start = applied.startSec;
        });
    result.invoked = QMetaObject::invokeMethod(&dialog, "onTrackClicked", Qt::DirectConnection);
    auto* button = dialog.findChild<QPushButton*>("cameraSolveButton");
    result.buttonEnabled = button && button->isEnabled();
    // The click enters QMessageBox::exec(). Answer only this dialog's visible
    // prompt, after its Yes button is ready. No fixed settling sleep or global
    // topLevelWidgets sweep; the context also cancels an unused callback.
    QTimer answerContext;
    QTimer::singleShot(0, &answerContext, [&] {
        QMessageBox* prompt = nullptr;
        result.promptReady = QTest::qWaitFor([&] {
            prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            return prompt && prompt->parentWidget() == &dialog && prompt->isVisible()
                && prompt->button(QMessageBox::Yes)
                && prompt->button(QMessageBox::Yes)->isEnabled();
        }, 3000);
        if (result.promptReady)
            prompt->button(QMessageBox::Yes)->click();
        else if (prompt && prompt->parentWidget() == &dialog)
            prompt->reject(); // Fail, but do not strand the acceptance process.
    });
    if (result.buttonEnabled) button->click();
    QObject::disconnect(connection);
    return result;
}

bool diagnosePlanar(const char* label, const PlanarRun& run, int expectedLoads)
{
    bool ok = run.invoked && run.loads == expectedLoads && run.buttonEnabled
        && run.promptReady && run.applied == 1 && run.poses.size() == 3
        && run.fps == 10 && run.start == 2.0;
    std::fprintf(stderr, "G3 %s: invoked=%d loads=%d (expected=%d) buttonEnabled=%d "
        "promptReady=%d applied=%d poses.size()=%lld (expected=3) "
        "appliedFps=%.17g (expected=10) appliedStart=%.17g (expected=2)\n",
        label, int(run.invoked), run.loads, expectedLoads, int(run.buttonEnabled),
        int(run.promptReady), run.applied, static_cast<long long>(run.poses.size()), run.fps, run.start);
    for (qsizetype frame = 0; frame < run.poses.size(); ++frame) {
        const auto& pose = run.poses[frame];
        double deviation = 0;
        for (int i = 0; i < 9; ++i) {
            const double delta = std::abs(pose.R[i] - (i % 4 == 0 ? 1.0 : 0.0));
            deviation = std::isfinite(delta) ? std::max(deviation, delta)
                                           : std::numeric_limits<double>::infinity();
        }
        const bool identity = pose.valid && pose.t.length() < 1e-3 && deviation < 1e-3;
        std::fprintf(stderr, "G3 %s pose[%lld]: valid=%d t.length()=%.17g "
            "R.maxDeviation=%.17g identity=%s (t,R < 0.001)\n",
            label, static_cast<long long>(frame), int(pose.valid), double(pose.t.length()),
            deviation, identity ? "PASS" : "FAIL");
        ok = ok && identity;
    }
    return ok;
}

}

int runCamtrackUiSelftest()
{
    selftest::GateTally tally;
    // A tracking run that hit the 60 s timeout fails its gate as "timeout". If its
    // worker did not stop either, gate() returns true and the selftest ends.
    bool timedOut = false, stuck = false;
    const auto gate = [&](int number, bool ok) {
        if (stuck) return abortStuck(tally, number);
        if (timedOut) tally.gate(number, false, "timeout");
        else tally.gate(number, ok);
        timedOut = false;
        return false;
    };
    QTemporaryDir temporary;
    const QString moving = temporary.filePath("moving.avi");
    const QString blank = temporary.filePath("blank.avi");
    const QString large = temporary.filePath("large.avi");
    const bool media = temporary.isValid() && writeClip(moving, {320, 240})
        && writeClip(blank, {320, 240}, true) && writeClip(large, {1440, 900});
    PlanarTrackerDialog dialog;
    dialog.setFeatureTrackingSource(moving, 10, 6);
    dialog.findChild<QSpinBox*>("featureMaxPoints")->setValue(40);
    int ticks = 0;
    bool tracked = media && runTracking(dialog, 1, 4, ticks, timedOut, stuck);
    int accurate = 0;
    const auto& tracks = dialog.featureTracks();
    for (const auto& track : tracks.tracks) {
        if (track.observations.size() != 4) continue;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const QPointF expected = track.observations.first().second + QPointF(2 * i, 0);
            const QPointF delta = track.observations[i].second - expected;
            ok = ok && track.observations[i].first == i && std::hypot(delta.x(), delta.y()) < 1.0;
        }
        if (ok) ++accurate;
    }
    const QString summary = dialog.findChild<QLabel*>("featureSummary")->text();
    if (gate(1, tracked && tracks.frameCount == 4 && tracks.tracks.size() <= 40
         && accurate >= 8 && ticks > 0 && dialog.featureStartFrame() == 1
         && summary.contains(QStringLiteral("追跡点数"))
         && summary.contains(QStringLiteral("フレーム数: 4"))
         && summary.contains(QStringLiteral("脱落率"))))
        return tally.finish();

    const auto rejectedWith = [&](const QString& text, int first, int last) {
        const bool rejected = !runTracking(dialog, first, last, ticks, timedOut, stuck);
        return rejected && dialog.featureTracks().tracks.isEmpty()
            && dialog.findChild<QLabel*>("featureSummary")->text().contains(text);
    };
    bool rejected = rejectedWith(QStringLiteral("最低2フレーム"), 2, 2)
        && rejectedWith(QStringLiteral("範囲が不正"), 4, 1);
    dialog.setFeatureTrackingSource(QStringLiteral("must-not-be-decoded.avi"), 10, sfm::kMaxBundleFrames + 1);
    rejected = rejectedWith(QStringLiteral("上限"), 0, sfm::kMaxBundleFrames)
        && dialog.featureAnalysisSize().isEmpty() && rejected;
    dialog.setFeatureTrackingSource(blank, 10, 6);
    rejected = rejectedWith(QStringLiteral("最低8点"), 0, 3) && rejected;
    dialog.setFeatureTrackingSource(large, 10, 6);
    const bool scaled = media && runTracking(dialog, 0, 2, ticks, timedOut, stuck);
    const QSize size = dialog.featureAnalysisSize();
    if (gate(2, rejected && scaled && std::min(size.width(), size.height()) <= 720
             && size.width() < 1440 && size.height() < 900))
        return tally.finish();

    // Diagnose the rejected fixture on both entry points. texture() keeps a fixed
    // 30px margin and its patches are at most 23px wide, so in a 320x240 frame the
    // columns past 291 and the rows past 211 stay flat gray. onResetCorners puts the
    // corners at 10%/90%, which leaves the bottom-right 32px patch mostly flat:
    // equal-SAD candidates move a stationary corner. Identity was therefore not a
    // valid oracle for that fixture, on any compiler.
    const QImage oldReference = texture({320, 240});
    const auto oldEager = runPlanar(oldReference, false);
    const auto oldLazy = runPlanar(oldReference, true);
    diagnosePlanar("original-fixture/eager (diagnostic only)", oldEager, 0);
    diagnosePlanar("original-fixture/lazy (diagnostic only)", oldLazy, 1);

    // Fill every pixel with deterministic, non-repeating texture, so no corner's
    // search region can be flat regardless of where onResetCorners places it.
    QImage reference(320, 240, QImage::Format_RGB32);
    std::mt19937 random(621);
    for (int y = 0; y < reference.height(); ++y)
        for (int x = 0; x < reference.width(); ++x) {
            const int value = int(random() % 256);
            reference.setPixel(x, y, qRgb(value, value, value));
        }
    const auto eager = runPlanar(reference, false);
    const auto lazy = runPlanar(reference, true);
    const bool eagerIdentity = diagnosePlanar("textured/eager", eager, 0);
    const bool lazyIdentity = diagnosePlanar("textured/lazy", lazy, 1);
    // This is entry-point parity, NOT a claim of cross-revision equivalence.
    // The old tracker and solver are called, never reimplemented in this test.
    // Separate source comparison against Wave base 4a68be6 confirms unchanged
    // PlanarTracker.cpp, CameraSolver.cpp, onCameraSolveClicked, and the original
    // onTrackClicked body after the added lazy-loading prefix.
    const auto sameOutput = [](const PlanarRun& a, const PlanarRun& b) {
        if (a.poses.size() != 3 || a.poses.size() != b.poses.size()
            || a.applied != 1 || b.applied != 1 || a.fps != b.fps || a.start != b.start)
            return false;
        for (qsizetype i = 0; i < a.poses.size(); ++i) {
            const auto& p = a.poses[i]; const auto& q = b.poses[i];
            if (p.valid != q.valid || p.t != q.t || p.n != q.n || p.residual != q.residual)
                return false;
            for (int j = 0; j < 9; ++j)
                if (p.R[j] != q.R[j]) return false;
        }
        return true;
    };
    const bool originalParity = sameOutput(oldEager, oldLazy);
    const bool texturedParity = sameOutput(eager, lazy);
    std::fprintf(stderr, "G3 entry-point parity: original=%d textured=%d "
        "textured identity: eager=%d lazy=%d\n", int(originalParity), int(texturedParity),
        int(eagerIdentity), int(lazyIdentity));
    gate(3, originalParity && texturedParity && eagerIdentity && lazyIdentity);

    sfm::MultiViewTracks boundary;
    boundary.frameCount = sfm::kMaxBundleFrames;
    QVector<camsolve::Pose> initial(boundary.frameCount);
    for (auto& pose : initial) pose.valid = true;
    QVector<QVector3D> points;
    const camsolve::Intrinsics k{100, 160, 120};
    for (int i = 0; i < sfm::kMaxBundlePoints; ++i) {
        const QVector3D p(float(i % 10), float(i / 10), 10);
        points.append(p);
        sfm::Track2D track;
        track.pointId = i;
        for (int frame = 0; frame < boundary.frameCount; ++frame)
            track.observations.append(qMakePair(frame, QPointF(k.f * p.x() / p.z() + k.cx,
                                                               k.f * p.y() / p.z() + k.cy)));
        boundary.tracks.append(track);
    }
    const auto accepted = sfm::bundleAdjust(boundary, initial, points, k, 0);
    auto excessFrames = boundary;
    ++excessFrames.frameCount;
    auto extraPoses = initial;
    extraPoses.append(initial.first());
    for (auto& track : excessFrames.tracks)
        track.observations.append(qMakePair(sfm::kMaxBundleFrames, track.observations.first().second));
    const auto frameRejected = sfm::bundleAdjust(excessFrames, extraPoses, points, k, 0);
    auto excessPoints = boundary;
    auto extraTrack = boundary.tracks.first(); extraTrack.pointId = sfm::kMaxBundlePoints;
    excessPoints.tracks.append(extraTrack);
    auto extraPoints = points; extraPoints.append(points.first());
    const auto pointRejected = sfm::bundleAdjust(excessPoints, initial, extraPoints, k, 0);
    const auto initialRejected = sfm::bundleAdjust(boundary, initial, extraPoints, k, 0);
    // Each limit has its own exact reason, so a rejection names the limit it hit.
    const QString frameReason = QStringLiteral("フレーム数が上限（%1フレーム）を超えています")
        .arg(sfm::kMaxBundleFrames);
    const QString pointReason = QStringLiteral("点数が上限（%1点）を超えています").arg(sfm::kMaxBundlePoints);
    const QString initialReason = QStringLiteral("初期点の数が上限（%1点）を超えています")
        .arg(sfm::kMaxBundlePoints);
    dialog.setFeatureTrackingSource(moving, 10, sfm::kMaxBundleFrames + 10);
    const bool frameOk = !frameRejected.valid && frameRejected.reason == frameReason;
    const bool pointOk = !pointRejected.valid && pointRejected.reason == pointReason;
    const bool initialOk = !initialRejected.valid && initialRejected.reason == initialReason;
    const QString limitsText = dialog.findChild<QLabel*>("featureLimits")->text();
    const bool limitsLabel = limitsText.contains(
        QStringLiteral("上限 %1 フレーム / %2 点").arg(sfm::kMaxBundleFrames).arg(sfm::kMaxBundlePoints));
    const int featureEnd = dialog.findChild<QSpinBox*>("featureEnd")->value();
    const int maxPoints = dialog.findChild<QSpinBox*>("featureMaxPoints")->maximum();
    std::fprintf(stderr, "G4 accepted=%d frameOk=%d frameReason=%s pointOk=%d pointReason=%s initialOk=%d initialReason=%s limitsLabel=%d featureEnd=%d maxPoints=%d\n",
        accepted.valid, frameOk, frameRejected.reason.toUtf8().constData(), pointOk,
        pointRejected.reason.toUtf8().constData(), initialOk, initialRejected.reason.toUtf8().constData(),
        limitsLabel, featureEnd, maxPoints);
    gate(4, accepted.valid && frameOk && pointOk && initialOk && limitsLabel
         && featureEnd == sfm::kMaxBundleFrames - 1 && maxPoints == sfm::kMaxBundlePoints);

    const auto summaryOf = [](PlanarTrackerDialog& d) {
        auto* label = d.findChild<QLabel*>(QStringLiteral("planarSummary"));
        return label ? label->text() : QStringLiteral("<planarSummary missing>");
    };

    {
        // G5: the dialog alone, in MainWindow's order of calls.
        PlanarTrackerDialog d;
        d.setFeatureTrackingSource(QStringLiteral("g5-source.avi"), 10, 30);
        d.findChild<QSpinBox*>("featureStart")->setValue(3);
        d.findChild<QSpinBox*>("featureEnd")->setValue(14);
        int loads = 0;
        d.setPlanarFrameLoader([&] { ++loads; return QList<QImage>{}; });
        const QString text = summaryOf(d);
        std::fprintf(stderr, "G5 summary=\"%s\" expected=%d (want 12) loads=%d\n",
                     qUtf8Printable(text), expectedFromSummary(text), loads);
        gate(5, text.contains(QStringLiteral("読み込み待ち")) && expectedFromSummary(text) == 12
             && !text.contains(QStringLiteral("投入: 0")) && loads == 0);
    }

    {
        // G6: clip A tracks 4 frames; reopening on clip B must not show A's count.
        PlanarTrackerDialog d;
        d.setReferenceFrame(reference);
        const QList<QImage> clipA{reference, reference, reference, reference};
        d.setPlanarFrameLoader([&] { return clipA; });
        const bool invoked = QMetaObject::invokeMethod(&d, "onTrackClicked", Qt::DirectConnection);
        const QString textA = summaryOf(d);
        const int trackedA = int(d.trackResult().size());
        QImage referenceB = reference.mirrored(true, false);
        d.setReferenceFrame(referenceB);
        d.setPlanarFrameLoader([&] { return QList<QImage>{referenceB}; });
        d.setFeatureTrackingSource(QStringLiteral("g6-clip-b.avi"), 10, 30);
        d.findChild<QSpinBox*>("featureStart")->setValue(0);
        d.findChild<QSpinBox*>("featureEnd")->setValue(6);
        const QString textB = summaryOf(d);
        std::fprintf(stderr, "G6 invoked=%d clipA tracked=%d (want 4) summaryA=\"%s\" "
                     "summaryB=\"%s\" expectedB=%d (want 7) containsA4=%d trackedB=%d\n",
                     int(invoked), trackedA, qUtf8Printable(textA), qUtf8Printable(textB),
                     expectedFromSummary(textB), int(textB.contains(QLatin1Char('4'))),
                     int(d.trackResult().size()));
        gate(6, invoked && trackedA == 4 && textA.contains(QStringLiteral("投入: 4"))
             && expectedFromSummary(textB) == 7 && !textB.contains(QLatin1Char('4'))
             && d.trackResult().isEmpty());
    }

    {
        // G7: MainWindow::openPlanarTrackerDialog, the real production path.
        const QString clipPath = temporary.filePath("g7-source.avi");
        const bool clipOk = temporary.isValid()
            && writeClip(clipPath, {320, 240}, false, 24, 48);
        MainWindow window;
        auto* timeline = window.findChild<Timeline*>("mainTimeline");
        bool selected = false;
        QString error;
        if (clipOk && timeline) {
            ClipInfo clip;
            clip.filePath = clipPath;
            clip.displayName = QStringLiteral("g7");
            clip.duration = 2.0;
            clip.inPoint = 0.0;
            clip.outPoint = 2.0;
            clip.speed = 1.0;
            clip.opacity = 1.0;
            timeline->videoTracks().first()->setClips(QVector<ClipInfo>{clip});
            timeline->refreshPlaybackSequence();
            selected = timeline->selectClipByIndex(false, 0, 0, &error);
        }
        // An unexpected QMessageBox would block invokeMethod; fail instead.
        QString modalText;
        QTimer modalGuard;
        modalGuard.setInterval(500);
        QObject::connect(&modalGuard, &QTimer::timeout, &modalGuard, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                modalText = box->text();
                std::fprintf(stderr, "G7 modal: %s\n", qUtf8Printable(modalText));
                box->reject();
            }
        });
        modalGuard.start();
        const bool invoked = selected
            && QMetaObject::invokeMethod(&window, "openPlanarTrackerDialog");
        modalGuard.stop();
        auto* d = window.findChild<PlanarTrackerDialog*>("planarTrackerDialog");
        const QString text = d ? summaryOf(*d) : QStringLiteral("<dialog missing>");
        const int start = d ? d->findChild<QSpinBox*>("featureStart")->value() : -1;
        const int end = d ? d->findChild<QSpinBox*>("featureEnd")->value() : -1;
        const int frameCount = d ? d->featureFrameCount() : -1;
        const int want = std::min(frameCount, end - start + 1);
        const int got = expectedFromSummary(text);
        std::fprintf(stderr, "G7 clipOk=%d selected=%d (%s) invoked=%d summary=\"%s\" "
                     "start=%d end=%d frameCount=%d want=%d got=%d\n",
                     int(clipOk), int(selected), qUtf8Printable(error), int(invoked), qUtf8Printable(text),
                     start, end, frameCount, want, got);
        if (!modalText.isEmpty()) {
            const QByteArray detail = "modal: " + modalText.toUtf8();
            tally.gate(7, false, detail.constData());
        } else {
            gate(7, invoked && d && text.contains(QStringLiteral("読み込み待ち"))
                 && got == want && got > 0 && !text.contains(QStringLiteral("投入: 0")));
        }
        if (d) d->hide();
    }

    {
        // G11: the feature path applies the reference-centred, scaled mapping and writes the
        // analysed focal length to the Fov track. Run at fov 800 (S > 100) and at the default
        // fov 60 (the control: S < 100, so a hard-coded 100 cannot pass), then once more with
        // an edited scale field.
        QVector<camsolve::Pose> truth;
        bool all = true;
        for (const double fov : {800.0, 60.0}) {
            const auto tracks11 = solveScene(truth, fov);
            MainWindow window;
            auto& camera = window.*member(Camera{});
            PlanarTrackerDialog d;
            d.setCameraSolveContext({1280, 720}, fov, 24, 5);
            d.setFeatureTrackingSource(QStringLiteral("synthetic"), 24, 240);
            d.setFeatureTrackingResult(tracks11, {1280, 720}, 0);
            int signals11 = 0;
            camsolve::CameraSolveApplied emitted;
            QObject::connect(&d, &PlanarTrackerDialog::cameraSolveApplied, &window,
                [&](const camsolve::CameraSolveApplied& applied) {
                    ++signals11;
                    emitted = applied;
                    (window.*member(Apply{}))(applied);
                });
            const bool solved = d.solveFeatureCamera();
            const double expectedS = referenceScale(emitted, fov);
            const double refTime = emitted.startSec; // pose 0 is the reference
            const auto ref = camera.getCameraAt(refTime);
            const double positionError = ref.position.length();
            const double targetError = (ref.target - QVector3D(0, 0, -1)).length();
            const double fovLate = camera.getCameraAt(refTime + 5.0 / 24).fov;
            const QJsonObject mapping = camera.toJson().value(QStringLiteral("solveMapping")).toObject();
            const double jsonS = mapping.value(QStringLiteral("unitScalePx")).toDouble(-1);
            auto* spin = d.findChild<QDoubleSpinBox*>(QStringLiteral("featureScale"));
            const double shown = spin ? spin->value() : -1;
            const bool scaleOk = std::isfinite(expectedS) && jsonS == emitted.mapping.unitScalePx
                && std::abs(jsonS - expectedS) <= 1e-9 * expectedS
                && spin && std::abs(shown - expectedS) <= 5e-4 + 1e-9
                && (fov != 60.0 || expectedS < 100);
            // The user edits the scale field; the next solve applies that value.
            if (spin) spin->setValue(25.0);
            const bool resolved = d.solveFeatureCamera();
            const double editedS = camera.toJson().value(QStringLiteral("solveMapping")).toObject()
                .value(QStringLiteral("unitScalePx")).toDouble(-1);
            const bool ok = solved && resolved && signals11 == 2 && positionError < 1e-4
                && targetError < 1e-4 && std::abs(ref.fov - fov) < 1e-9
                && std::abs(fovLate - fov) < 1e-9 && scaleOk && editedS == 25.0
                && std::abs(mapping.value(QStringLiteral("focalPx")).toDouble() - fov) < 1e-9;
            std::fprintf(stderr, "G11 fov=%g solved=%d resolved=%d signals=%d position|.|=%.3g "
                "targetErr=%.3g fovRef=%.12g fovLate=%.12g unitScalePx=%.17g expected=%.17g "
                "shown=%.6g editedS=%.17g ok=%d\n", fov, int(solved), int(resolved), signals11,
                positionError, targetError, ref.fov, fovLate, jsonS, expectedS, shown, editedS, int(ok));
            all = all && ok;
        }
        gate(11, all);
    }

    {
        // G12 (control): the planar path keeps the legacy pose mapping, leaves the Fov track
        // alone and writes no solveMapping; a rejected feature solve leaves the undo slot alone.
        const auto planar = runPlanar(reference, false);
        QVector<camsolve::Pose> truth;
        solveScene(truth, 800);
        MainWindow window;
        auto& camera = window.*member(Camera{});
        const auto before = camera.toJson();
        const double fovBefore = camera.camera().fov;
        // Non-trivial poses, so a feature-style mapping could not coincide with the legacy one.
        camsolve::CameraSolveApplied legacy;
        legacy.poses = truth;
        legacy.fps = 24; legacy.startSec = 5;
        legacy.mapping.legacy = true;
        (window.*member(Apply{}))(legacy);
        bool matches = camera.track(Camera3DProperty::PositionX)
            && camera.track(Camera3DProperty::PositionX)->count() == truth.size();
        double maxDelta = 0;
        for (qsizetype i = 0; matches && i < truth.size(); ++i) {
            const auto expected = camsolve::poseToCameraState(truth[i], camera.camera());
            const auto actual = camera.getCameraAt(5 + double(i) / 24);
            maxDelta = std::max(maxDelta, double((actual.position - expected.position).length()));
            maxDelta = std::max(maxDelta, double((actual.target - expected.target).length()));
        }
        const bool fovKept = std::abs(camera.getCameraAt(5).fov - fovBefore) < 1e-9
            && std::abs(camera.getCameraAt(5.2).fov - fovBefore) < 1e-9;
        const bool noMapping = !camera.toJson().contains(QStringLiteral("solveMapping"))
            && !camera.solveMapping().has_value();
        const bool moved = camera.toJson() != before;
        // A feature solve with a non-positive focal length is refused: camera and undo slot unchanged.
        const auto slotBefore = window.*member(UndoSlot{});
        const auto cameraBefore = camera.toJson();
        camsolve::CameraSolveApplied rejected = legacy;
        rejected.mapping.legacy = false;
        rejected.mapping.focalPx = 0;
        (window.*member(Apply{}))(rejected);
        const bool refused = camera.toJson() == cameraBefore && (window.*member(UndoSlot{})) == slotBefore;
        // Feature -> planar transition: the legacy apply drops the feature solveMapping and the
        // undo slot restores it.
        camsolve::CameraSolveApplied feature = legacy;
        feature.mapping.legacy = false;
        feature.mapping.unitScalePx = 50;
        feature.mapping.focalPx = 800;
        feature.mapping.analysisSize = QSize(1280, 720);
        feature.mapping.canvasSize = QSize(1280, 720);
        (window.*member(Apply{}))(feature);
        const auto featureJson = camera.toJson();
        const bool featureMapped = camera.solveMapping().has_value()
            && featureJson.contains(QStringLiteral("solveMapping"));
        (window.*member(Apply{}))(legacy);
        const bool mappingDropped = !camera.toJson().contains(QStringLiteral("solveMapping"))
            && !camera.solveMapping().has_value();
        const auto restoreSlot = window.*member(UndoSlot{});
        camera = Camera3D{};
        camera.fromJson(restoreSlot);
        const bool mappingRestored = camera.solveMapping().has_value()
            && camera.toJson() == featureJson;
        const bool transition = featureMapped && mappingDropped && mappingRestored;
        const bool emittedLegacy = planar.solve.mapping.legacy && planar.solve.points.isEmpty()
            && planar.solve.poses.size() == 3;
        std::fprintf(stderr, "G12 planarEmittedLegacy=%d moved=%d maxDelta=%.3g fovKept=%d "
            "noMapping=%d refused=%d transition=%d/%d/%d\n", int(emittedLegacy), int(moved), maxDelta,
            int(fovKept), int(noMapping), int(refused), int(featureMapped), int(mappingDropped),
            int(mappingRestored));
        gate(12, emittedLegacy && moved && matches && maxDelta < 1e-5 && fovKept && noMapping && refused
             && transition);
    }

    {
        // G8: closing the dialog mid-analysis blocks for at most ~5 s, and the
        // worker's late callbacks land on its context, not the dead dialog.
        auto semaphore = std::make_shared<QSemaphore>(0);
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto orphaned = std::make_shared<std::atomic<int>>(0);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        const auto savedGrab = PlanarTrackerDialog::featureGrabFunctionForTest();
        const QImage board = checkerboard();
        PlanarTrackerDialog::featureGrabFunctionForTest() =
            [semaphore, entered, board](const QString&, double, const QSize&) {
                entered->store(true);
                semaphore->acquire();
                return board;
            };
        PlanarTrackerDialog::featureOrphanCallbackHookForTest() = [orphaned] { ++*orphaned; };
        auto* d = new PlanarTrackerDialog;
        d->setFeatureTrackingSource(QStringLiteral("g8-seam.avi"), 10, 6);
        d->findChild<QSpinBox*>("featureStart")->setValue(0);
        d->findChild<QSpinBox*>("featureEnd")->setValue(3);
        d->findChild<QSpinBox*>("featureMaxPoints")->setValue(40);
        d->findChild<QPushButton*>("featureTrackButton")->click();
        QPointer<QThread> worker = d->featureWorkerThread();
        QPointer<QObject> ctx = d->featureWorkerContext();
        QObject probe;
        if (worker)
            QObject::connect(worker, &QThread::finished, &probe,
                             [finished] { finished->store(true); }, Qt::DirectConnection);
        const bool blocked = worker && ctx && QTest::qWaitFor([&] { return entered->load(); }, 10000);
        g_capturedWarning.clear();
        g_previousHandler = qInstallMessageHandler(captureWarning);
        QElapsedTimer clock;
        clock.start();
        delete d;
        const qint64 destructorMs = clock.elapsed();
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
        const bool workerAliveAfterDelete = !worker.isNull() && !finished->load();
        semaphore->release(100);
        const bool workerFinished = QTest::qWaitFor([&] { return finished->load(); }, 10000);
        const bool cleaned = QTest::qWaitFor([&] { return ctx.isNull() && worker.isNull(); }, 10000);
        const int lateCallbacks = orphaned->load();
        PlanarTrackerDialog::featureGrabFunctionForTest() = savedGrab;
        PlanarTrackerDialog::featureOrphanCallbackHookForTest() = {};
        std::fprintf(stderr, "G8 blocked=%d destructorMs=%lld (want 4500..6500) warning=\"%s\" "
                     "workerAliveAfterDelete=%d workerFinished=%d ctxDeleted=%d workerDeleted=%d "
                     "lateCallbacks=%d (want > 0)\n",
                     int(blocked), static_cast<long long>(destructorMs), qUtf8Printable(g_capturedWarning),
                     int(workerAliveAfterDelete), int(workerFinished), int(ctx.isNull()),
                     int(worker.isNull()), lateCallbacks);
        gate(8, blocked && destructorMs >= 4500 && destructorMs <= 6500
             && !g_capturedWarning.isEmpty() && workerAliveAfterDelete && workerFinished
             && cleaned && lateCallbacks > 0);
    }

    // G9 and G10 block the worker in the grab seam until released.
    const auto blockGrab = [](const std::shared_ptr<QSemaphore>& semaphore,
                              const std::shared_ptr<std::atomic<bool>>& entered) {
        const QImage board = checkerboard();
        PlanarTrackerDialog::featureGrabFunctionForTest() =
            [semaphore, entered, board](const QString&, double, const QSize&) {
                entered->store(true);
                semaphore->acquire();
                return board;
            };
    };

    {
        // G9: a controlled timeout whose worker does not stop marks the run stuck,
        // later runs leave the dialog alone, and abortStuck fails the gate and
        // lets the selftest finish its tally.
        auto semaphore = std::make_shared<QSemaphore>(0);
        auto entered = std::make_shared<std::atomic<bool>>(false);
        const auto savedGrab = PlanarTrackerDialog::featureGrabFunctionForTest();
        blockGrab(semaphore, entered);
        PlanarTrackerDialog d;
        d.setFeatureTrackingSource(QStringLiteral("g9-seam.avi"), 10, 6);
        d.findChild<QSpinBox*>("featureMaxPoints")->setValue(40);
        int localTicks = 0;
        bool localTimedOut = false, localStuck = false;
        const bool tracked = runTracking(d, 0, 3, localTicks, localTimedOut, localStuck, 300, 300);
        const bool runningWhenStuck = d.featureTrackingRunning();
        bool laterTimedOut = false;
        QElapsedTimer laterClock;
        laterClock.start();
        const bool laterRan = runTracking(d, 0, 3, localTicks, laterTimedOut, localStuck);
        const qint64 laterMs = laterClock.elapsed();
        std::ostringstream captured;
        std::streambuf* const previous = std::cerr.rdbuf(captured.rdbuf());
        selftest::GateTally scratch;
        const bool aborted = abortStuck(scratch, 1);
        const int scratchFailed = scratch.finish(nullptr, false);
        std::cerr.rdbuf(previous);
        const QString abortText = QString::fromStdString(captured.str()).trimmed()
            .replace(QLatin1Char('\n'), QStringLiteral(" | "));
        semaphore->release(100);
        const bool stopped = QTest::qWaitFor([&] { return !d.featureTrackingRunning(); }, 10000);
        PlanarTrackerDialog::featureGrabFunctionForTest() = savedGrab;
        const bool abortOk = aborted && scratchFailed == 1 && scratch.passed == 0
            && abortText.contains(QStringLiteral("FAIL G1 timeout (worker still running"))
            && abortText.contains(QStringLiteral("summary: 0 PASS, 1 FAIL, 0 SKIP"));
        std::fprintf(stderr, "G9 entered=%d tracked=%d timedOut=%d stuck=%d runningWhenStuck=%d "
                     "laterRan=%d laterTimedOut=%d laterMs=%lld (want < 1000) abort=\"%s\" "
                     "scratchFailed=%d stoppedAfterRelease=%d\n",
                     int(entered->load()), int(tracked), int(localTimedOut), int(localStuck),
                     int(runningWhenStuck), int(laterRan), int(laterTimedOut),
                     static_cast<long long>(laterMs), qUtf8Printable(abortText), scratchFailed,
                     int(stopped));
        gate(9, entered->load() && !tracked && localTimedOut && localStuck && runningWhenStuck
             && !laterRan && !laterTimedOut && laterMs < 1000 && abortOk && stopped);
    }

    {
        // G10: the application-exit wait is registered when the worker starts.
        // aboutToQuit is emitted through the meta-object (the real signal; this
        // selftest is the last user of the QApplication). The handler interrupts
        // and waits ~2 s without deleting; after the worker is deleted, a second
        // emission finds no handler.
        auto semaphore = std::make_shared<QSemaphore>(0);
        auto entered = std::make_shared<std::atomic<bool>>(false);
        const auto savedGrab = PlanarTrackerDialog::featureGrabFunctionForTest();
        blockGrab(semaphore, entered);
        PlanarTrackerDialog d;
        d.setFeatureTrackingSource(QStringLiteral("g10-seam.avi"), 10, 6);
        d.findChild<QSpinBox*>("featureMaxPoints")->setValue(40);
        d.findChild<QSpinBox*>("featureStart")->setValue(0);
        d.findChild<QSpinBox*>("featureEnd")->setValue(3);
        d.findChild<QPushButton*>("featureTrackButton")->click();
        QPointer<QThread> worker = d.featureWorkerThread();
        const bool blocked = worker && QTest::qWaitFor([&] { return entered->load(); }, 10000);
        QElapsedTimer clock;
        clock.start();
        const bool emitted = QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection);
        const qint64 quitMs = clock.elapsed();
        const bool interrupted = worker && worker->isInterruptionRequested();
        const bool stillRunning = worker && worker->isRunning() && d.featureTrackingRunning();
        semaphore->release(100);
        const bool stopped = QTest::qWaitFor([&] { return !d.featureTrackingRunning(); }, 10000);
        const bool deleted = QTest::qWaitFor([&] { return worker.isNull(); }, 10000);
        clock.restart();
        const bool emittedAgain = QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection);
        const qint64 secondMs = clock.elapsed();
        PlanarTrackerDialog::featureGrabFunctionForTest() = savedGrab;
        std::fprintf(stderr, "G10 blocked=%d emitted=%d quitMs=%lld (want 1500..3500) interrupted=%d "
                     "stillRunning=%d stopped=%d workerDeleted=%d emittedAgain=%d secondMs=%lld "
                     "(want < 500)\n",
                     int(blocked), int(emitted), static_cast<long long>(quitMs), int(interrupted),
                     int(stillRunning), int(stopped), int(deleted), int(emittedAgain),
                     static_cast<long long>(secondMs));
        gate(10, blocked && emitted && quitMs >= 1500 && quitMs <= 3500 && interrupted
             && stillRunning && stopped && deleted && emittedAgain && secondMs < 500);
    }
    return tally.finish();
}
