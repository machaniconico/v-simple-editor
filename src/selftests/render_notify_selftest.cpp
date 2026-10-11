// --selftest=render-notify: export-complete notification (default OFF) wired once at
// MainWindow::ensureRenderQueue. Batch (dedicated queue) and RenderInPlace (local queue) are out of scope.
#include "SelftestGate.h"
#include "../BatchExportQueue.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "../RenderQueueDialog.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpToolRegistry.h"
#include "../Timeline.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QVector>

#include <cstdio>
#include <string>

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
    QString encoderReason;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderReason)) {
        const QByteArray reason = encoderReason.toUtf8();
        for (int n = 1; n <= 4; ++n) tally.skip(n, reason.constData());
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
    // RenderQueueDialog keeps its private queue (byte-identity, pinned below);
    // (b) MCP export_video on a fresh window (queue not yet created) goes through the same ensureRenderQueue queue and notifies once.
    {
        setNotify(true);
        MainWindow window;
        const bool done = fixture && run(window, {work.filePath("g3.mp4"),
            work.filePath("no-such-dir/g3-fail.mp4")}, nullptr, nullptr);
        window.openRenderQueueDialog();
        const bool dialogPrivate = window.m_renderQueueDialog && window.m_renderQueue
            && window.m_renderQueueDialog->queue() != window.m_renderQueue;
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
        std::fprintf(stderr, "[render-notify] G3 done=%d twoJobs=%d dialogPrivate=%d counter=%d message=%s\n"
            "[render-notify] G3 mcp queueNullBefore=%d notError=%d jobId=%s queueSet=%d queueSame=%d "
            "hasJob=%d done=%d counter=%d message=%s\n",
            done, twoJobs, dialogPrivate, window.renderCompleteNotificationCountForTest, qPrintable(msg),
            queueNullBefore, notError, qPrintable(jobId), queueSet, queueSame, hasJob, mcpDone,
            mcpWindow.renderCompleteNotificationCountForTest, qPrintable(mcpMsg));
        tally.gate(3, twoJobs && dialogPrivate && mcpOk);
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
    restore();
    return tally.finish("render-notify");
}
