#include "../BatchExportDialog.h"
#include "../BatchExportQueue.h"
#include "../Timeline.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    return std::fabs(lhs - rhs) <= 1.0e-9;
}

bool containsState(const QVector<batchexport::TaskState> &states,
                   batchexport::TaskState wanted)
{
    return std::find(states.cbegin(), states.cend(), wanted) != states.cend();
}

batchexport::TaskState taskState(const batchexport::Queue &queue,
                                 const QString &id)
{
    for (const batchexport::ExportTask &task : queue.tasks()) {
        if (task.id == id)
            return task.state;
    }
    return batchexport::TaskState::Failed;
}

bool runQueueContractGate(QString *detail)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        if (detail)
            *detail = QStringLiteral("temporary directory unavailable");
        return false;
    }

    batchexport::ExportSettings settings;
    settings.preset = QStringLiteral("1080p");

    const batchexport::ExportTask firstTask =
        BatchExportDialog::makeFileProjectTask(
            tempDir.filePath(QStringLiteral("missing-first.veditor")),
            tempDir.filePath(QStringLiteral("first.mp4")), settings);
    const batchexport::ExportTask secondTask =
        BatchExportDialog::makeFileProjectTask(
            tempDir.filePath(QStringLiteral("missing-second.veditor")),
            tempDir.filePath(QStringLiteral("second.mp4")), settings);

    batchexport::Queue queue;
    const QString firstId = queue.addTask(firstTask);
    const QString secondId = queue.addTask(secondTask);
    if (firstId.isEmpty() || secondId.isEmpty()) {
        if (detail)
            *detail = QStringLiteral("queue did not register two tasks");
        return false;
    }

    QHash<QString, QVector<batchexport::TaskState>> states;
    QSet<QString> progressIds;
    int queueFinishedCount = 0;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged,
                     &queue, [&](const QString &id,
                                 batchexport::TaskState state) {
        states[id].append(state);
    });
    QObject::connect(&queue, &batchexport::Queue::taskProgress,
                     &queue, [&](const QString &id, int) {
        progressIds.insert(id);
    });
    QObject::connect(&queue, &batchexport::Queue::queueFinished,
                     &queue, [&]() { ++queueFinishedCount; });

    QEventLoop firstLoop;
    QTimer firstTimeout;
    firstTimeout.setSingleShot(true);
    bool firstTerminal = false;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged,
                     &firstLoop, [&](const QString &id,
                                     batchexport::TaskState state) {
        if (id == firstId
            && (state == batchexport::TaskState::Done
                || state == batchexport::TaskState::Failed)) {
            firstTerminal = true;
            firstLoop.quit();
        }
    });
    QObject::connect(&firstTimeout, &QTimer::timeout,
                     &firstLoop, &QEventLoop::quit);

    queue.start();
    queue.pause();
    if (!firstTerminal) {
        firstTimeout.start(10000);
        firstLoop.exec();
    }
    firstTimeout.stop();

    const bool signalContract =
        queue.metaObject()->indexOfSignal("taskProgress(QString,int)") >= 0
        && queue.metaObject()->indexOfSignal(
               "taskStateChanged(QString,batchexport::TaskState)") >= 0
        && queue.metaObject()->indexOfSignal("queueFinished()") >= 0;
    const bool firstEmittedExpectedStates =
        containsState(states.value(firstId), batchexport::TaskState::Running)
        && containsState(states.value(firstId), batchexport::TaskState::Failed);
    const bool pauseHeld =
        firstTerminal
        && taskState(queue, firstId) == batchexport::TaskState::Failed
        && taskState(queue, secondId) == batchexport::TaskState::Queued
        && queueFinishedCount == 0;

    QEventLoop secondLoop;
    QTimer secondTimeout;
    secondTimeout.setSingleShot(true);
    bool finishedAfterResume = false;
    QObject::connect(&queue, &batchexport::Queue::queueFinished,
                     &secondLoop, [&]() {
        finishedAfterResume = true;
        secondLoop.quit();
    });
    QObject::connect(&secondTimeout, &QTimer::timeout,
                     &secondLoop, &QEventLoop::quit);
    queue.resume();
    if (!finishedAfterResume) {
        secondTimeout.start(10000);
        secondLoop.exec();
    }
    secondTimeout.stop();

    const bool resumedAndFinished =
        finishedAfterResume
        && queueFinishedCount == 1
        && taskState(queue, secondId) == batchexport::TaskState::Failed
        && containsState(states.value(secondId), batchexport::TaskState::Running)
        && containsState(states.value(secondId), batchexport::TaskState::Failed);

    // Invalid project files fail before a render frame exists, so no progress
    // value is expected here. The real success-path taskProgress=100 contract
    // remains covered by the existing batchexport selftest when test_assets is
    // present; this gate verifies the signal exists and pause/advance ordering.
    Q_UNUSED(progressIds);
    if (!(signalContract && firstEmittedExpectedStates
          && pauseHeld && resumedAndFinished)) {
        if (detail) {
            *detail = QStringLiteral(
                "signals=%1 firstStates=%2 pauseHeld=%3 resumed=%4")
                .arg(signalContract)
                .arg(firstEmittedExpectedStates)
                .arg(pauseHeld)
                .arg(resumedAndFinished);
        }
        return false;
    }
    return true;
}

} // namespace

int runBatchExportE2ESelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral(
                "[batchexport-e2e] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral(
                "[batchexport-e2e] FAIL G%1 %2%3")
                .arg(gate)
                .arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString()
                                      : QStringLiteral(": ") + detail);
        }
    };

    if (!QApplication::instance()) {
        qCritical().noquote() << QStringLiteral(
            "[batchexport-e2e] FAIL G1 QApplication is unavailable");
        return 1;
    }

    // The selftest creates a real Timeline model but never shows a window or
    // invokes a dialog. This makes the pointer transport check genuine while
    // keeping the E2E path GUI-free.
    Timeline timeline;
    BatchExportDialog::CurrentProjectContext context;
    context.projectPath = QStringLiteral("current-project.veditor");
    context.timeline = &timeline;
    context.width = 640;
    context.height = 360;
    context.fps = 24.0;
    context.startUs = 1'000'000;
    context.endUs = 3'000'000;

    batchexport::ExportSettings currentSettings;
    currentSettings.preset = QStringLiteral("1080p");
    currentSettings.codec = QStringLiteral("h264");
    currentSettings.videoCodec = QStringLiteral("libx264");
    currentSettings.videoBitrateKbps = 12345;
    currentSettings.audioCodec = QStringLiteral("aac");
    currentSettings.audioBitrateKbps = 160;
    const batchexport::ExportTask currentTask =
        BatchExportDialog::makeCurrentProjectTask(
            context, QStringLiteral("current.mp4"), currentSettings);
    check(1, "current-project task carries live Timeline",
          currentTask.timeline == &timeline
              && currentTask.source == batchexport::TaskSource::CurrentProject);

    const RenderJob currentJob =
        batchexport::Queue::buildRenderJob(currentTask);
    const double currentFps = qvariant_cast<double>(
        currentJob.exportConfig.value("fps").toVariant());
    check(2, "explicit fps reaches RenderJob exportConfig",
          currentJob.timeline == &timeline
              && nearlyEqual(currentFps, 24.0)
              && currentJob.exportConfig.value("videoBitrate").toInt() == 12345
              && currentJob.exportConfig.value("audioBitrate").toInt() == 160);

    batchexport::ExportSettings unspecifiedSettings;
    unspecifiedSettings.preset = QStringLiteral("1080p");
    const batchexport::ExportTask unspecifiedTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("file-project.veditor"),
            QStringLiteral("file.mp4"), unspecifiedSettings);
    const RenderJob unspecifiedJob =
        batchexport::Queue::buildRenderJob(unspecifiedTask);
    check(3, "unspecified fps leaves exportConfig key absent",
          unspecifiedJob.exportConfig.contains("fps") == false);

    batchexport::ExportTask explicitSizeTask = currentTask;
    explicitSizeTask.width = 640;
    explicitSizeTask.height = 360;
    explicitSizeTask.preset = QStringLiteral("1080p");
    const RenderJob explicitSizeJob =
        batchexport::Queue::buildRenderJob(explicitSizeTask);

    batchexport::ExportSettings presetSettings;
    presetSettings.preset = QStringLiteral("720p");
    const batchexport::ExportTask presetSizeTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("preset-project.veditor"),
            QStringLiteral("preset.mp4"), presetSettings);
    const RenderJob presetSizeJob =
        batchexport::Queue::buildRenderJob(presetSizeTask);
    check(4, "explicit geometry wins and preset geometry is fallback",
          explicitSizeJob.exportConfig.value("width").toInt() == 640
              && explicitSizeJob.exportConfig.value("height").toInt() == 360
              && presetSizeJob.exportConfig.value("width").toInt() == 1280
              && presetSizeJob.exportConfig.value("height").toInt() == 720);

    const batchexport::ExportTask fileTask =
        BatchExportDialog::makeFileProjectTask(
            QStringLiteral("saved-project.veditor"),
            QStringLiteral("saved.mp4"), currentSettings);
    const bool fileRouteIsExplicit =
        fileTask.timeline == nullptr
        && fileTask.source == batchexport::TaskSource::File;
    QString detail;
    check(5, "file route retains its source distinction",
          fileRouteIsExplicit, QStringLiteral("source=%1 timeline=%2")
              .arg(static_cast<int>(fileTask.source))
              .arg(fileTask.timeline != nullptr));

    detail.clear();
    check(6, "queue signals and paused advance contract",
          runQueueContractGate(&detail), detail);

    qInfo().noquote() << QStringLiteral(
        "[batchexport-e2e] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
