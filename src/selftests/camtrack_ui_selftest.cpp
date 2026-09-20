#include "../PlanarTrackerDialog.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QAbstractButton>
#include <QEventLoop>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <limits>

namespace {
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
        painter.fillRect(x, y, 8 + int(rng() % 16), 8 + int(rng() % 16), QColor(value, value, value));
    }
    return image;
}

bool writeClip(const QString& path, QSize size, bool blank = false)
{
    libavcore::EncodeRequest request;
    request.width = size.width(); request.height = size.height();
    request.fps = 10; request.fpsNum = 10; request.fpsDen = 1;
    request.videoBitrateBits = 8000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    const QImage base = texture(size);
    for (int i = 0; i < 6; ++i) {
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
bool runTracking(PlanarTrackerDialog& dialog, int first, int last, int& ticks)
{
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
    heartbeat.start(); timeout.start(60000);
    dialog.findChild<QPushButton*>("featureTrackButton")->click();
    if (!finished) loop.exec();
    QObject::disconnect(connection);
    return finished && success;
}

struct PlanarRun {
    bool invoked = false;
    int loads = 0;
    bool buttonEnabled = false;
    bool promptReady = false;
    int applied = 0;
    double fps = 0, start = 0;
    QVector<camsolve::Pose> poses;
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
        &dialog, [&](const QVector<camsolve::Pose>& poses, double fps, double start) {
            ++result.applied;
            result.poses = poses; result.fps = fps; result.start = start;
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
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        if (ok) ++passed; else ++failed;
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
    bool tracked = media && runTracking(dialog, 1, 4, ticks);
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
    gate(1, tracked && tracks.frameCount == 4 && tracks.tracks.size() <= 40
         && accurate >= 8 && ticks > 0 && dialog.featureStartFrame() == 1
         && summary.contains(QStringLiteral("追跡点数"))
         && summary.contains(QStringLiteral("フレーム数: 4"))
         && summary.contains(QStringLiteral("脱落率")));

    const auto rejectedWith = [&](const QString& text, int first, int last) {
        const bool rejected = !runTracking(dialog, first, last, ticks);
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
    const bool scaled = media && runTracking(dialog, 0, 2, ticks);
    const QSize size = dialog.featureAnalysisSize();
    gate(2, rejected && scaled && std::min(size.width(), size.height()) <= 720
         && size.width() < 1440 && size.height() < 900);

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
    const QString reason = QStringLiteral("上限は%1フレーム、%2点です")
        .arg(sfm::kMaxBundleFrames).arg(sfm::kMaxBundlePoints);
    dialog.setFeatureTrackingSource(moving, 10, sfm::kMaxBundleFrames + 10);
    gate(4, accepted.valid && !frameRejected.valid && frameRejected.reason == reason
         && !pointRejected.valid && pointRejected.reason == reason
         && !initialRejected.valid && initialRejected.reason == reason
         && dialog.findChild<QLabel*>("featureLimits")->text().contains(
             QStringLiteral("上限 %1 フレーム / %2 点").arg(sfm::kMaxBundleFrames).arg(sfm::kMaxBundlePoints))
         && dialog.findChild<QSpinBox*>("featureEnd")->value() == sfm::kMaxBundleFrames - 1
         && dialog.findChild<QSpinBox*>("featureMaxPoints")->maximum() == sfm::kMaxBundlePoints);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
