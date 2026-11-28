// --selftest=render-notify: export-complete notification (default OFF) wired once at
// MainWindow::ensureRenderQueue (RenderQueueDialog shares that queue since US-1125). Batch (dedicated
// queue) and RenderInPlace (local queue) are out of scope.
#include "SelftestGate.h"
#include "../AcesColor.h"
#include "../BatchExportQueue.h"
#include "../ExportDialog.h"
#include "../Exporter.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "../RenderQueueDialog.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpToolRegistry.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"

#include <functional>
#include <memory>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProgressDialog>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <cstdio>
#include <string>

double exporter_loudnessGainDb();

namespace {

const char *const kNotifyKey = "notify/renderComplete";
const QString kNotifyText = QStringLiteral("書き出しが完了しました");

bool writeVideo(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = 64;
    request.height = 64;
    request.fps = request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    request.videoBitrateBits = 200000;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(Qt::blue);
    for (int i = 0; i < 10; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize();
}

RenderJob makeJob(Timeline *timeline, const QString &out)
{
    batchexport::ExportTask t;
    t.projectPath = QStringLiteral("render-notify.veditor");
    t.outputPath = out;
    t.preset = QStringLiteral("720p");
    t.timeline = timeline;
    t.width = t.height = 64;
    t.fps = 10.0;
    t.codec = QStringLiteral("mpeg4");
    t.videoCodec = QStringLiteral("mpeg4");
    t.videoBitrateKbps = 200;
    t.audioCodec = QStringLiteral("aac");
    t.audioBitrateKbps = 192;
    return batchexport::Queue::buildRenderJob(t);
}

void setNotify(bool on)
{
    QSettings settings("VSimpleEditor", "Preferences");
    settings.setValue(kNotifyKey, on);
    settings.sync();
}

} // namespace

int runRenderNotifySelftest()
{
    selftest::GateTally tally;
    QSettings settings("VSimpleEditor", "Preferences");
    const QVariant previous = settings.value(kNotifyKey);
    const auto restore = [&]() {
        if (previous.isValid()) settings.setValue(kNotifyKey, previous);
        else settings.remove(kNotifyKey);
        settings.sync();
    };

    QTemporaryDir work;
    const QString video = work.filePath("notify-video.mp4");

    // G6: a queued job keeps the ACES pipeline that was set when it was added; a later
    // setAcesPipeline is only the default for later jobs, and a pipeline the caller put on
    // the job is kept. Detects the worker reading the queue's live pipeline (HEAD: RenderJob
    // had no acesPipeline) and addJob not copying it (mutation in US-1108-mutation.txt).
    // Needs no encoder (nothing is rendered), so it runs before the mpeg4 check.
    const auto gateAcesPerJob = [&]() {
        RenderQueue queue;
        QVector<RenderJob> seen;
        QObject::connect(&queue, &RenderQueue::jobsChanged, &queue, [&]() { seen = queue.jobs(); });
        aces::AcesPipeline on;
        on.enabled = true;
        on.input = aces::ColorSpace::Rec709;
        on.output = aces::ColorSpace::sRGB;
        queue.setAcesPipeline(on);
        queue.addJob(makeJob(nullptr, work.filePath("g6-a.mp4")));
        queue.setAcesPipeline(aces::AcesPipeline{});
        queue.addJob(makeJob(nullptr, work.filePath("g6-b.mp4")));
        RenderJob explicitJob = makeJob(nullptr, work.filePath("g6-c.mp4"));
        explicitJob.acesPipeline = on;
        queue.addJob(explicitJob);
        const auto describe = [](const RenderJob &job) {
            if (!job.acesPipeline) return QStringLiteral("unset");
            return QStringLiteral("enabled=%1 input=%2 output=%3").arg(job.acesPipeline->enabled)
                .arg(aces::colorSpaceName(job.acesPipeline->input), aces::colorSpaceName(job.acesPipeline->output));
        };
        const auto isOn = [&](const RenderJob &job) {
            return job.acesPipeline && job.acesPipeline->enabled && job.acesPipeline->input == on.input
                && job.acesPipeline->output == on.output;
        };
        const bool three = seen.size() == 3;
        const bool firstKept = three && isOn(seen[0]);
        const bool secondOff = three && seen[1].acesPipeline && !seen[1].acesPipeline->enabled;
        const bool explicitKept = three && isOn(seen[2]);
        std::fprintf(stderr, "[render-notify] G6 jobs=%lld first=%s second=%s explicit=%s\n",
            static_cast<long long>(seen.size()),
            qPrintable(three ? describe(seen[0]) : QString()),
            qPrintable(three ? describe(seen[1]) : QString()),
            qPrintable(three ? describe(seen[2]) : QString()));
        tally.gate(6, firstKept && secondOff && explicitKept);
    };

    // G7: the dialog's "ジョブ追加" lands on the shared queue and states the project's ACES pipeline
    // and a non-zero loudness gain on its job; a 0 dB gain adds no key (RenderQueue::addJob rule).
    // The queue's own defaults are set to the opposite (ACES off, 0 dB), so a job that merely
    // inherits them (no explicit provider, or HEAD's private queue) fails. Renders nothing.
    const auto gateDialogShare = [&]() {
        const double previousGain = exporter_loudnessGainDb();
        Exporter gainSetter;
        MainWindow window;
        window.hide();
        aces::AcesPipeline on;
        on.enabled = true;
        on.input = aces::ColorSpace::Rec709;
        on.output = aces::ColorSpace::sRGB;
        window.m_acesPipeline = on;
        window.openRenderQueueDialog();
        RenderQueue *queue = window.m_renderQueue;
        const bool shared = queue && window.m_renderQueueDialog
            && window.m_renderQueueDialog->queue() == queue;
        QPushButton *addButton = window.m_renderQueueDialog
            ? window.m_renderQueueDialog->findChild<QPushButton *>(QStringLiteral("renderQueueAddJobButton"))
            : nullptr;
        bool drove = false;
        // Adds one job through the modal sub-dialog (blank source -> the live Timeline).
        const auto addViaDialog = [&](const QString &out) {
            drove = false;
            // One bounded retry when the modal has not appeared yet; never an unbounded wait.
            auto drive = std::make_shared<std::function<void(int)>>();
            const std::weak_ptr<std::function<void(int)>> weakDrive = drive;
            QObject *timerCtx = window.m_renderQueueDialog;   // timers die with the dialog
            *drive = [&, out, weakDrive, timerCtx](int retries) {
                QWidget *modal = QApplication::activeModalWidget();
                if (!modal && retries > 0) {
                    QTimer::singleShot(50, timerCtx, [weakDrive, retries]() {
                        if (const auto self = weakDrive.lock()) (*self)(retries - 1);
                    });
                    return;
                }
                QLineEdit *edit = modal ? modal->findChild<QLineEdit *>(QStringLiteral("addJobOutputEdit")) : nullptr;
                if (!edit) {
                    std::fprintf(stderr, "[render-notify] G7 add-dialog input not found\n");
                    if (modal) QMetaObject::invokeMethod(modal, "reject");
                    return;
                }
                edit->setText(out);
                drove = QMetaObject::invokeMethod(modal, "accept");
            };
            QTimer::singleShot(0, timerCtx, [drive]() { (*drive)(1); });
            addButton->click();
            *drive = nullptr;   // drop the stored closure; stale timers find nothing
            return drove;
        };
        bool firstOk = false, secondOk = false;
        int before = -1, afterFirst = -1, afterSecond = -1;
        QString firstDesc, secondDesc;
        double gainFirst = 99.0;
        bool secondHasKey = true;
        bool foreignKept = false, ownGone = false;
        QString secondUuid;
        QVector<RenderJob> seen;
        QMetaObject::Connection seenConn;
        if (shared && addButton && window.m_timeline) {
            seenConn = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() { seen = queue->jobs(); });
            queue->setAcesPipeline(aces::AcesPipeline{});
            queue->setLoudnessGainDb(0.0);
            gainSetter.setLoudnessGainDb(-3.0);
            before = queue->jobs().size();
            const bool added = addViaDialog(work.filePath("g7-a.mp4"));
            afterFirst = seen.size();
            if (added && afterFirst == before + 1 && !seen.isEmpty()) {
                const RenderJob job = seen.last();
                gainFirst = job.exportConfig.value(QStringLiteral("loudnessGainDb")).toDouble(99.0);
                firstDesc = job.acesPipeline ? QStringLiteral("enabled=%1 input=%2 output=%3")
                    .arg(job.acesPipeline->enabled).arg(aces::colorSpaceName(job.acesPipeline->input),
                        aces::colorSpaceName(job.acesPipeline->output)) : QStringLiteral("unset");
                firstOk = job.acesPipeline && job.acesPipeline->enabled
                    && job.acesPipeline->input == on.input && job.acesPipeline->output == on.output
                    && exporter_loudnessGainDb() == -3.0 && gainFirst == exporter_loudnessGainDb();
            }
            // Negative side: ACES off and 0 dB -> the job follows the project (disabled, no key).
            window.m_acesPipeline = aces::AcesPipeline{};
            queue->setAcesPipeline(on);
            gainSetter.setLoudnessGainDb(0.0);
            const bool addedAgain = addViaDialog(work.filePath("g7-b.mp4"));
            afterSecond = seen.size();
            if (addedAgain && afterSecond == afterFirst + 1 && !seen.isEmpty()) {
                const RenderJob job = seen.last();
                secondUuid = job.uuid;
                secondHasKey = job.exportConfig.contains(QStringLiteral("loudnessGainDb"));
                secondDesc = job.acesPipeline ? QStringLiteral("enabled=%1").arg(job.acesPipeline->enabled)
                    : QStringLiteral("unset");
                secondOk = job.acesPipeline && !job.acesPipeline->enabled && !secondHasKey;
            }
            // 削除 only removes jobs this dialog added: a pending job put straight on the shared
            // queue (GUI / MCP) must survive, a dialog-added one must go.
            RenderJob foreign = makeJob(nullptr, work.filePath("g7-foreign.mp4"));
            queue->addJob(foreign);
            const QString foreignUuid = seen.isEmpty() ? QString() : seen.last().uuid;
            const auto removeViaDialog = [&](const QString &uuid) {
                auto *table = window.m_renderQueueDialog->findChild<QTableWidget *>(QStringLiteral("renderQueueJobTable"));
                auto *removeBtn = window.m_renderQueueDialog->findChild<QPushButton *>(
                    QStringLiteral("renderQueueRemoveJobButton"));
                if (!table || !removeBtn) return;
                for (int row = 0; row < table->rowCount(); ++row) {
                    QTableWidgetItem *item = table->item(row, 0);
                    if (item && item->data(Qt::UserRole).toString() == uuid) {
                        table->selectRow(row);
                        table->setCurrentCell(row, 0);
                        removeBtn->click();
                        return;
                    }
                }
            };
            const auto hasJob = [&](const QString &uuid) {
                for (const RenderJob &j : seen) if (j.uuid == uuid) return true;
                return false;
            };
            const QString ownUuid = secondUuid;
            removeViaDialog(foreignUuid);
            foreignKept = !foreignUuid.isEmpty() && hasJob(foreignUuid);
            removeViaDialog(ownUuid);
            ownGone = !ownUuid.isEmpty() && !hasJob(ownUuid);
            QObject::disconnect(seenConn);
        }
        gainSetter.setLoudnessGainDb(previousGain);
        std::fprintf(stderr, "[render-notify] G7 shared=%d button=%d jobs %d->%d->%d seen=%d first{%s gain=%g} "
            "second{%s hasKey=%d} remove{foreignKept=%d ownGone=%d}\n", shared, addButton != nullptr, before, afterFirst, afterSecond, static_cast<int>(seen.size()),
            qPrintable(firstDesc), gainFirst, qPrintable(secondDesc), secondHasKey, foreignKept, ownGone);
        tally.gate(7, shared && firstOk && secondOk && foreignKept && ownGone);
    };

    QString encoderReason;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderReason)) {
        const QByteArray reason = encoderReason.toUtf8();
        for (int n = 1; n <= 5; ++n) tally.skip(n, reason.constData());
        tally.skip(8, reason.constData());
        gateAcesPerJob();
        gateDialogShare();
        return tally.finish("render-notify");
    }
    const bool fixture = work.isValid() && writeVideo(video);
    Timeline tl;
    ClipInfo picture;
    picture.filePath = video;
    picture.duration = picture.outPoint = 1.0;
    tl.restoreFromProject(QVector<QVector<ClipInfo>>{{picture}},
        QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);

    // Runs `outs` through window.ensureRenderQueue() and waits for allCompleted.
    // midCounter/midMessage record the notification state when the first job ends.
    const auto run = [&](MainWindow &window, const QStringList &outs, int *midCounter,
                         bool *midMessage) {
        RenderQueue *queue = window.ensureRenderQueue();
        QSignalSpy all(queue, &RenderQueue::allCompleted);
        bool first = true;
        QObject::connect(queue, &RenderQueue::jobCompletedUuid, queue,
            [&](QString, bool, QString) {
                if (!first) return;
                first = false;
                if (midCounter) *midCounter = window.renderCompleteNotificationCountForTest;
                if (midMessage)
                    *midMessage = window.statusBar()->currentMessage().contains(kNotifyText);
            });
        for (const QString &out : outs) queue->addJob(makeJob(&tl, out));
        queue->start();
        return all.wait(120000) || !all.isEmpty();
    };
    const auto messageOf = [](MainWindow &window) { return window.statusBar()->currentMessage(); };

    // G1: OFF (default) -> no message, counter 0. Detects notifying while the setting is OFF.
    {
        // Remove the key (not setNotify(false)) so the true default is what is under test.
        bool keyPresent = true;
        {
            QSettings s("VSimpleEditor", "Preferences");
            s.remove(kNotifyKey);
            s.sync();
            keyPresent = s.contains(kNotifyKey);
        }
        MainWindow window;
        const bool done = fixture && run(window, {work.filePath("g1.mp4")}, nullptr, nullptr);
        const QString msg = messageOf(window);
        std::fprintf(stderr, "[render-notify] G1 fixture=%d keyPresent=%d done=%d counter=%d message=%s\n",
            fixture, keyPresent, done, window.renderCompleteNotificationCountForTest, qPrintable(msg));
        tally.gate(1, !keyPresent && done && !msg.contains(kNotifyText)
            && window.renderCompleteNotificationCountForTest == 0);
    }
    // G2: ON -> "(1 件、失敗 0 件)" and counter 1. Detects a missing allCompleted hookup.
    {
        setNotify(true);
        MainWindow window;
        const bool done = fixture && run(window, {work.filePath("g2.mp4")}, nullptr, nullptr);
        const QString msg = messageOf(window);
        std::fprintf(stderr, "[render-notify] G2 done=%d counter=%d message=%s\n",
            done, window.renderCompleteNotificationCountForTest, qPrintable(msg));
        tally.gate(2, done
            && msg.contains(QStringLiteral("書き出しが完了しました (1 件、失敗 0 件)"))
            && window.renderCompleteNotificationCountForTest == 1);
    }
    // G3: (a) one good + one failing job (missing output directory) -> "(2 件、失敗 1 件)"; the
    // RenderQueueDialog shares the ensureRenderQueue queue since US-1125 (HEAD pinned the
    // opposite, queue() != m_renderQueue, which this flipped pin replaces);
    // (b) MCP export_video on a fresh window (queue not yet created) goes through the same ensureRenderQueue queue and notifies once.
    {
        setNotify(true);
        MainWindow window;
        const bool done = fixture && run(window, {work.filePath("g3.mp4"),
            work.filePath("no-such-dir/g3-fail.mp4")}, nullptr, nullptr);
        window.openRenderQueueDialog();
        const bool dialogShared = window.m_renderQueueDialog && window.m_renderQueue
            && window.m_renderQueueDialog->queue() == window.m_renderQueue;
        const QString msg = messageOf(window);
        const bool twoJobs = done
            && msg.contains(QStringLiteral("書き出しが完了しました (2 件、失敗 1 件)"))
            && window.renderCompleteNotificationCountForTest == 1;

        MainWindow mcpWindow;
        mcpWindow.hide();
        const bool queueNullBefore = mcpWindow.m_renderQueue == nullptr;
        bool notError = false, queueSet = false, queueSame = false, hasJob = false;
        bool mcpDone = false;
        QSet<QString> seenJobs;
        QString jobId, mcpMsg;
        if (fixture && mcpWindow.m_timeline) {
            ClipInfo mcpClip;
            mcpClip.filePath = video;
            mcpClip.duration = mcpClip.outPoint = 1.0;
            mcpWindow.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{mcpClip}},
                QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
            mcp::McpToolRegistry registry;
            mcp::McpEditorTools tools(&mcpWindow, &registry);
            tools.registerWriteTools();
            // The deferred addJob/start only runs for the window's own tools (see McpEditorTools).
            mcpWindow.m_mcpTools = &tools;
            mcp::McpProtocol protocol(&registry, mcp::ServerInfo{});
            const QJsonObject request{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), 1}, {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("export_video")},
                    {QStringLiteral("arguments"), QJsonObject{
                        {QStringLiteral("outputPath"), work.filePath("g3-mcp.mp4")},
                        {QStringLiteral("width"), 64}, {QStringLiteral("height"), 64},
                        {QStringLiteral("fps"), 10}, {QStringLiteral("videoCodec"), QStringLiteral("mpeg4")},
                        {QStringLiteral("videoBitrate"), 200}}}}}};
            const QJsonObject response = QJsonDocument::fromJson(protocol.handleMessage(
                QJsonDocument(request).toJson(QJsonDocument::Compact))).object();
            // The queue exists now (ensureRenderQueue) but the job is added by a deferred
            // QTimer::singleShot(0), so this capture is connected before it.
            if (mcpWindow.m_renderQueue) {
                RenderQueue *capQueue = mcpWindow.m_renderQueue;
                QObject::connect(capQueue, &RenderQueue::jobsChanged, capQueue, [&seenJobs, capQueue]() {
                    for (const RenderJob &job : capQueue->jobs()) seenJobs.insert(job.uuid);
                });
            }
            const QJsonObject result = response.value(QStringLiteral("result")).toObject();
            notError = !result.value(QStringLiteral("isError")).toBool(true);
            const QJsonArray content = result.value(QStringLiteral("content")).toArray();
            const QJsonObject payload = result.value(QStringLiteral("structuredContent")).toObject();
            jobId = payload.value(QStringLiteral("jobId")).toString();
            if (jobId.isEmpty() && !content.isEmpty())
                jobId = QJsonDocument::fromJson(content.first().toObject()
                    .value(QStringLiteral("text")).toString().toUtf8()).object()
                    .value(QStringLiteral("jobId")).toString();
            RenderQueue *queue = mcpWindow.m_renderQueue;
            queueSet = queue != nullptr;
            queueSame = queue && queue == mcpWindow.ensureRenderQueue();
            if (queue) {
                QSignalSpy all(queue, &RenderQueue::allCompleted);
                mcpDone = all.wait(120000) || !all.isEmpty();
                hasJob = !jobId.isEmpty() && seenJobs.contains(jobId);
            }
            mcpMsg = messageOf(mcpWindow);
            mcpWindow.m_mcpTools = nullptr;
        }
        const bool mcpOk = queueNullBefore && notError && !jobId.isEmpty() && queueSet && queueSame
            && hasJob && mcpDone
            && mcpWindow.renderCompleteNotificationCountForTest == 1
            && mcpMsg.contains(QStringLiteral("書き出しが完了しました (1 件"));
        std::fprintf(stderr, "[render-notify] G3 done=%d twoJobs=%d dialogShared=%d counter=%d message=%s\n"
            "[render-notify] G3 mcp queueNullBefore=%d notError=%d jobId=%s queueSet=%d queueSame=%d "
            "hasJob=%d done=%d counter=%d message=%s\n",
            done, twoJobs, dialogShared, window.renderCompleteNotificationCountForTest, qPrintable(msg),
            queueNullBefore, notError, qPrintable(jobId), queueSet, queueSame, hasJob, mcpDone,
            mcpWindow.renderCompleteNotificationCountForTest, qPrintable(mcpMsg));
        tally.gate(3, twoJobs && dialogShared && mcpOk);
    }
    // G4 (negative control): ON, two jobs. When the first job ends the second is still queued:
    // no notification yet (jobCompleted must not notify); exactly one after allCompleted.
    {
        setNotify(true);
        MainWindow window;
        int midCounter = -1;
        bool midMessage = true;
        const bool done = fixture && run(window, {work.filePath("g4a.mp4"),
            work.filePath("g4b.mp4")}, &midCounter, &midMessage);
        std::fprintf(stderr, "[render-notify] G4 done=%d midCounter=%d midMessage=%d finalCounter=%d "
            "message=%s\n", done, midCounter, midMessage,
            window.renderCompleteNotificationCountForTest, qPrintable(messageOf(window)));
        tally.gate(4, done && midCounter == 0 && !midMessage
            && window.renderCompleteNotificationCountForTest == 1
            && messageOf(window).contains(QStringLiteral("(2 件、失敗 0 件)")));
    }
    // G5: on the shared queue the normal / mobile export progress dialog follows only the
    // uuid buildExportRenderJob gave its job, and Cancel / cancelJob touch only that job.
    // Detects the dialog reacting to another job's jobProgressUuid (HEAD connected without
    // the uuid; mutation in US-1108-mutation.txt) and Cancel stopping the whole queue.
    {
        MainWindow window;
        window.hide();
        RenderQueue *queue = window.ensureRenderQueue();
        ExportConfig cfg;
        cfg.outputPath = work.filePath("g5-gui.mp4");
        cfg.width = cfg.height = 64;
        cfg.fps = 10;
        cfg.videoCodec = QStringLiteral("mpeg4");
        cfg.videoBitrate = 200;
        const RenderJob guiJob = window.buildExportRenderJob(cfg, MainWindow::ExportEntry::Normal);
        const bool uuidSet = !QUuid::fromString(guiJob.uuid).isNull();
        window.showExportProgressDialog(guiJob.uuid, QStringLiteral("g5"), QStringLiteral("Cancel"),
                                        [](bool, const QString &) {});
        QPointer<QProgressDialog> dialog =
            window.findChild<QProgressDialog *>(QStringLiteral("exportProgressDialog"));
        const bool dialogFound = !dialog.isNull();

        RenderJob other = makeJob(&tl, work.filePath("g5-other.mp4"));
        QVector<RenderJob> seen;
        QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() { seen = queue->jobs(); });
        queue->addJob(other);
        queue->addJob(guiJob);
        const bool queuedBoth = seen.size() == 2 && seen[0].uuid == other.uuid
            && seen[1].uuid == guiJob.uuid;

        // While `other` renders: an unknown uuid changes nothing, the dialog's Cancel removes
        // its pending job only, and cancelJob(other) cancels the running job.
        bool cancelsDone = false, unknownNoop = false, pendingRemoved = false, stillRunning = false;
        bool runningCancelled = false;
        const auto statusOf = [&](const QString &uuid) {
            for (const RenderJob &job : seen)
                if (job.uuid == uuid) return static_cast<int>(job.status);
            return -1;
        };
        QObject::connect(queue, &RenderQueue::jobProgressUuid, queue, [&](QString uuid, int) {
            if (cancelsDone || uuid != other.uuid) return;
            cancelsDone = true;
            seen = queue->jobs();
            queue->cancelJob(QStringLiteral("render-notify-g5-no-such-job"));
            unknownNoop = seen.size() == 2
                && statusOf(other.uuid) == static_cast<int>(RenderJobStatus::Rendering)
                && statusOf(guiJob.uuid) == static_cast<int>(RenderJobStatus::Pending);
            if (dialog) emit dialog->canceled();
            pendingRemoved = seen.size() == 1 && statusOf(guiJob.uuid) == -1;
            stillRunning = queue->isRunning()
                && statusOf(other.uuid) == static_cast<int>(RenderJobStatus::Rendering);
            queue->cancelJob(other.uuid);
            runningCancelled = statusOf(other.uuid) == static_cast<int>(RenderJobStatus::Cancelled);
        });
        QSignalSpy all(queue, &RenderQueue::allCompleted);
        queue->start();
        const bool done = fixture && (all.wait(120000) || !all.isEmpty());
        // A finished (cancelled) job is not touched again.
        queue->cancelJob(other.uuid);
        const bool finishedNoop = seen.size() == 1
            && statusOf(other.uuid) == static_cast<int>(RenderJobStatus::Cancelled);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool dialogClosed = dialog.isNull();
        const int realProgress = window.progressDialogUpdateCountForTest(guiJob.uuid);

        // The other job's progress must not move a dialog; its own uuid does (positive control).
        window.showExportProgressDialog(guiJob.uuid, QStringLiteral("g5"), QStringLiteral("Cancel"),
                                        [](bool, const QString &) {});
        QPointer<QProgressDialog> probe =
            window.findChild<QProgressDialog *>(QStringLiteral("exportProgressDialog"));
        const int valueBefore = probe ? probe->value() : -2;
        emit queue->jobProgressUuid(other.uuid, 37);
        const int otherCount = window.progressDialogUpdateCountForTest(guiJob.uuid);
        const int otherValue = probe ? probe->value() : -2;
        emit queue->jobProgressUuid(guiJob.uuid, 41);
        const int ownCount = window.progressDialogUpdateCountForTest(guiJob.uuid);
        const int ownValue = probe ? probe->value() : -2;
        delete probe.data();
        std::fprintf(stderr, "[render-notify] G5 uuidSet=%d uuid=%s dialogFound=%d queuedBoth=%d "
            "done=%d realProgressUpdates=%d\n"
            "[render-notify] G5 cancelJob unknown->noop=%d dialogCancel pending->removed=%d "
            "queueStillRunning=%d running->cancelled=%d finished->noop=%d dialogClosed=%d\n"
            "[render-notify] G5 probe valueBefore=%d otherUuid count=%d value=%d ownUuid count=%d value=%d\n",
            uuidSet, qPrintable(guiJob.uuid), dialogFound, queuedBoth,
            done, realProgress, unknownNoop, pendingRemoved, stillRunning, runningCancelled,
            finishedNoop, dialogClosed, valueBefore, otherCount, otherValue, ownCount, ownValue);
        tally.gate(5, uuidSet && dialogFound && queuedBoth && done && cancelsDone && realProgress == 0
            && unknownNoop && pendingRemoved && stillRunning && runningCancelled && finishedNoop
            && dialogClosed && otherCount == 0 && otherValue == valueBefore
            && ownCount == 1 && ownValue == 41);
    }
    // G8: the dialog's "選択ジョブを中止" (renderQueueCancelJobButton) cancels only the selected
    // job: (1) a pending row leaves the list and jobs(), the first stays Pending (HEAD: no
    // button); (2) a Rendering row becomes Cancelled while the queue keeps running (real
    // fixture render, observed in jobProgressUuid like G5); (3) "キュー全体を停止" stops the
    // queue (isRunning false) and the pending job stays (regression).
    {
        MainWindow window;
        window.hide();
        window.openRenderQueueDialog();
        RenderQueue *queue = window.m_renderQueue;
        RenderQueueDialog *dlg = window.m_renderQueueDialog;
        auto *table = dlg ? dlg->findChild<QTableWidget *>(QStringLiteral("renderQueueJobTable")) : nullptr;
        auto *cancelBtn = dlg ? dlg->findChild<QPushButton *>(QStringLiteral("renderQueueCancelJobButton")) : nullptr;
        QPushButton *stopBtn = nullptr;
        if (dlg)
            for (QPushButton *b : dlg->findChildren<QPushButton *>())
                if (b->text() == QStringLiteral("キュー全体を停止")) stopBtn = b;
        QVector<RenderJob> seen;
        QMetaObject::Connection seenConn;
        QSignalSpy completed(queue, &RenderQueue::jobCompletedUuid);
        if (queue)
            seenConn = QObject::connect(queue, &RenderQueue::jobsChanged, queue,
                                        [&]() { seen = queue->jobs(); });
        const auto statusOf = [&](const QString &uuid) {
            for (const RenderJob &job : seen)
                if (job.uuid == uuid) return static_cast<int>(job.status);
            return -1;
        };
        // Condition wait for the worker's finishCurrentJob (jobCompletedUuid) of one job; returns
        // its success flag through `ok` and whether it arrived at all.
        const auto waitCompleted = [&](const QString &uuid, bool *ok = nullptr) {
            QElapsedTimer t;
            t.start();
            for (;;) {
                for (const QList<QVariant> &args : completed)
                    if (args.value(0).toString() == uuid) {
                        if (ok) *ok = args.value(1).toBool();
                        return true;
                    }
                if (t.elapsed() >= 60000) return false;
                completed.wait(200);
            }
        };
        const auto selectRowOf = [&](const QString &uuid) {
            for (int row = 0; table && row < table->rowCount(); ++row) {
                QTableWidgetItem *item = table->item(row, 0);
                if (item && item->data(Qt::UserRole).toString() == uuid) {
                    table->selectRow(row);
                    table->setCurrentCell(row, 0);
                    return true;
                }
            }
            return false;
        };
        const int pend = static_cast<int>(RenderJobStatus::Pending);
        const int rend = static_cast<int>(RenderJobStatus::Rendering);
        const int canc = static_cast<int>(RenderJobStatus::Cancelled);
        const bool ui = queue && dlg && table && cancelBtn && stopBtn;

        // (1) two pending jobs, never started: cancel the second.
        bool pendOk = false;
        int pendJobs = -1, pendRows = -1;
        if (ui) {
            RenderJob a = makeJob(nullptr, work.filePath("g8-a.mp4"));
            RenderJob b = makeJob(nullptr, work.filePath("g8-b.mp4"));
            queue->addJob(a);
            queue->addJob(b);
            const bool selected = selectRowOf(b.uuid);
            const bool enabled = cancelBtn->isEnabled();
            if (selected && enabled) cancelBtn->click();
            pendJobs = seen.size();
            pendRows = table->rowCount();
            pendOk = selected && enabled && pendJobs == 1 && pendRows == 1
                && statusOf(a.uuid) == pend && statusOf(b.uuid) == -1;
            queue->removeJob(a.uuid);
        }

        // (2) + (3): X renders, Y waits. Cancel X from its row, then (after X ends) a second
        // run stops the whole queue with Y pending.
        bool rendEnabled = false, rendCancelled = false, rendRunning = false, rendDone = false;
        bool stopFired = false, stopIdle = false, stopKept = false, stopDone = false;
        bool xOk = true, yOk = true, x2Ok = true, yEnded = true;
        if (ui && fixture) {
            RenderJob x = makeJob(&tl, work.filePath("g8-x.mp4"));
            RenderJob y = makeJob(&tl, work.filePath("g8-y.mp4"));
            queue->addJob(x);
            queue->addJob(y);
            QMetaObject::Connection conn = QObject::connect(queue, &RenderQueue::jobProgressUuid, queue,
                [&](QString uuid, int) {
                    if (rendEnabled || uuid != x.uuid) return;
                    if (statusOf(x.uuid) != rend || !selectRowOf(x.uuid)) return;
                    rendEnabled = cancelBtn->isEnabled();
                    cancelBtn->click();
                    rendCancelled = statusOf(x.uuid) == canc;
                    rendRunning = queue->isRunning();
                });
            queue->start();
            bool xArrived = waitCompleted(x.uuid, &xOk);
            rendDone = xArrived && !xOk && statusOf(x.uuid) == canc;
            QObject::disconnect(conn);
            // y may have started after x ended: stop, then wait for its completion before clear().
            const bool yRendering = statusOf(y.uuid) == rend;
            queue->stop();
            if (yRendering) yEnded = waitCompleted(y.uuid, &yOk);
            queue->clear();

            RenderJob x2 = makeJob(&tl, work.filePath("g8-x2.mp4"));
            RenderJob y2 = makeJob(&tl, work.filePath("g8-y2.mp4"));
            queue->addJob(x2);
            queue->addJob(y2);
            conn = QObject::connect(queue, &RenderQueue::jobProgressUuid, queue,
                [&](QString uuid, int) {
                    if (stopFired || uuid != x2.uuid) return;
                    stopFired = true;
                    stopBtn->click();
                    stopIdle = !queue->isRunning();
                    stopKept = statusOf(y2.uuid) == pend;
                });
            queue->start();
            stopDone = waitCompleted(x2.uuid, &x2Ok) && !x2Ok;
            QObject::disconnect(conn);
            queue->stop();
            queue->clear();
        }
        QObject::disconnect(seenConn);
        std::fprintf(stderr, "[render-notify] G8 completions xSuccess=%d yEnded=%d ySuccess=%d x2Success=%d\n",
                     xOk, yEnded, yOk, x2Ok);
        std::fprintf(stderr, "[render-notify] G8 ui=%d (1) pending jobs=%d rows=%d ok=%d\n"
            "[render-notify] G8 (2) rendering enabled=%d cancelled=%d running=%d done=%d\n"
            "[render-notify] G8 (3) stop fired=%d idle=%d pendingKept=%d done=%d\n",
            ui, pendJobs, pendRows, pendOk, rendEnabled, rendCancelled, rendRunning, rendDone,
            stopFired, stopIdle, stopKept, stopDone);
        tally.gate(8, ui && fixture && pendOk && rendEnabled && rendCancelled && rendRunning
            && rendDone && yEnded && stopFired && stopIdle && stopKept && stopDone);
    }
    gateAcesPerJob();
    gateDialogShare();
    restore();
    return tally.finish("render-notify");
}
