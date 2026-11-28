// --selftest=matte-export: exercise luma matte through live, project-file,
// and MCP RenderQueue exports. G2 is the no-matte negative control. G5 uses
// the real HTTP MCP endpoint on VEDITOR_MCP_PORT and leaves mcpAutoStart,
// mcpToken, and RecentFiles unchanged on the normal path.
#include "SelftestGate.h"
#include "../MainWindow.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../mcp/McpHttpServer.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtTest/QTest>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kFps = 10;
enum class Pattern { Black, HalfBlackWhite, Red };

bool writeClip(const QString &path, Pattern pattern, QString *error)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = kWidth;
    request.height = kHeight;
    request.fps = request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    request.videoBitrateBits = 600000;
    libavcore::FrameEncoder encoder;
    if (const auto failure = encoder.open(request)) {
        *error = QString::fromStdString(*failure);
        return false;
    }
    QImage frame(kWidth, kHeight, QImage::Format_RGB888);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            frame.setPixelColor(x, y, pattern == Pattern::Black ? Qt::black
                : pattern == Pattern::Red ? Qt::red
                : x < kWidth / 2 ? Qt::black : Qt::white);
    for (int i = 0; i < kFps; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            *error = QStringLiteral("FrameEncoder::pushFrame failed at %1").arg(i);
            return false;
        }
    }
    if (const auto failure = encoder.finalize()) {
        *error = QString::fromStdString(*failure);
        return false;
    }
    return true;
}

ClipInfo clip(const QString &path)
{
    ClipInfo result;
    result.filePath = path;
    result.displayName = QFileInfo(path).fileName();
    result.duration = result.outPoint = 1.0;
    return result;
}

TimelineTrackMatteEntry lumaEntry()
{
    TimelineTrackMatteEntry entry;
    entry.matteType = TrackMatteType::LumaMatte;
    entry.matteSourceClipId = trackMatteClipKey(1, 0);
    return entry;
}

RenderJob jobFor(Timeline *timeline, const QString &project, const QString &output)
{
    RenderJob job;
    job.timeline = timeline;
    job.projectFilePath = project;
    job.outputPath = output;
    job.codec = QStringLiteral("mpeg4");
    job.width = kWidth;
    job.height = kHeight;
    job.bitrateBps = 600000;
    job.endUs = 1000000;
    job.exportConfig = QJsonObject{{QStringLiteral("fps"), kFps},
                                   {QStringLiteral("videoCodec"), QStringLiteral("mpeg4")},
                                   {QStringLiteral("videoBitrate"), 600}};
    return job;
}

struct ExportResult {
    bool done = false;
    bool success = false;
    bool sawJob = false;
    QString error;
    QImage frame;
};

ExportResult exportQueue(const RenderJob &job)
{
    RenderQueue queue;
    ExportResult result;
    // jobs() is captured synchronously in the slot installed before addJob.
    QObject::connect(&queue, &RenderQueue::jobsChanged, &queue, [&] {
        for (const RenderJob &seen : queue.jobs())
            if (seen.outputPath == job.outputPath) result.sawJob = true;
    });
    QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &queue,
        [&](const QString &, bool success, const QString &error) {
            result.done = true;
            result.success = success;
            result.error = error;
        });
    QSignalSpy all(&queue, &RenderQueue::allCompleted);
    queue.addJob(job);
    queue.start();
    const bool completed = !all.isEmpty() || all.wait(120000);
    result.done &= completed;
    if (!completed) queue.stop();
    if (result.done && result.success)
        result.frame = libavcore::grabFrameAt(job.outputPath, 0.5);
    return result;
}

struct Halves {
    double leftR = -1, leftG = -1, leftB = -1;
    double rightR = -1, rightG = -1, rightB = -1;
    bool valid = false;
};

Halves measure(const QImage &frame)
{
    Halves halves;
    if (frame.size() != QSize(kWidth, kHeight)) return halves;
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    qint64 sums[2][3] = {};
    int counts[2] = {};
    // The sampled regions start at least eight pixels inside each half.
    for (int y = 8; y < kHeight - 8; ++y) {
        for (int x = 8; x < kWidth - 8; ++x) {
            if (x >= 24 && x < 40) continue;
            const int side = x < kWidth / 2 ? 0 : 1;
            const QColor color = rgb.pixelColor(x, y);
            sums[side][0] += color.red();
            sums[side][1] += color.green();
            sums[side][2] += color.blue();
            ++counts[side];
        }
    }
    if (!counts[0] || !counts[1]) return halves;
    halves.leftR = double(sums[0][0]) / counts[0];
    halves.leftG = double(sums[0][1]) / counts[0];
    halves.leftB = double(sums[0][2]) / counts[0];
    halves.rightR = double(sums[1][0]) / counts[1];
    halves.rightG = double(sums[1][1]) / counts[1];
    halves.rightB = double(sums[1][2]) / counts[1];
    halves.valid = true;
    return halves;
}

bool near(double measured, double wanted)
{
    return std::abs(measured - wanted) <= 6.0;
}

bool blackLeftRedRight(const Halves &h)
{
    return h.valid && near(h.leftR, 0) && near(h.leftG, 0) && near(h.leftB, 0)
        && near(h.rightR, 255) && near(h.rightG, 0) && near(h.rightB, 0);
}

bool blackBoth(const Halves &h)
{
    return h.valid && near(h.leftR, 0) && near(h.leftG, 0) && near(h.leftB, 0)
        && near(h.rightR, 0) && near(h.rightG, 0) && near(h.rightB, 0);
}

std::string detail(const Halves &h, const QString &extra)
{
    return QStringLiteral("left=(%1,%2,%3) right=(%4,%5,%6) %7")
        .arg(h.leftR, 0, 'f', 2).arg(h.leftG, 0, 'f', 2).arg(h.leftB, 0, 'f', 2)
        .arg(h.rightR, 0, 'f', 2).arg(h.rightG, 0, 'f', 2).arg(h.rightB, 0, 'f', 2)
        .arg(extra).toStdString();
}

bool equalPixels(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return false;
    const QImage first = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage second = b.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < first.height(); ++y)
        if (std::memcmp(first.constScanLine(y), second.constScanLine(y),
                        static_cast<std::size_t>(first.width() * 4)))
            return false;
    return true;
}

bool equalFiles(const QString &a, const QString &b)
{
    QFile first(a), second(b);
    return first.open(QIODevice::ReadOnly) && second.open(QIODevice::ReadOnly)
        && first.readAll() == second.readAll();
}

// Call the MainWindow-owned MCP tools over their real local protocol endpoint.
QJsonObject callMcp(mcp::McpHttpServer *server, const QString &name,
                    const QJsonObject &arguments)
{
    if (!server || !server->isRunning()) return {};
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(server->endpointUrl()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + server->token().toUtf8());
    const QJsonObject call{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), 1}, {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), name},
            {QStringLiteral("arguments"), arguments}}}};
    QNetworkReply *reply = network.post(request, QJsonDocument(call).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    if (!reply->isFinished()) loop.exec();
    if (!reply->isFinished()) reply->abort();
    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return response.value(QStringLiteral("result")).toObject();
}

bool toolOk(const QJsonObject &result)
{
    return !result.isEmpty() && !result.value(QStringLiteral("isError")).toBool(true)
        && result.value(QStringLiteral("structuredContent")).toObject()
            .value(QStringLiteral("ok")).toBool();
}

bool configureMatte(MainWindow &window)
{
    bool configured = false;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            message->done(QMessageBox::Ok);
            return;
        }
        auto *type = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteTypeCombo"));
        auto *source = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteSourceCombo"));
        if (!type || !source) { dialog->reject(); return; }
        const int typeIndex = type->findData(int(TrackMatteType::LumaMatte));
        const int sourceIndex = source->findData(trackMatteClipKey(1, 0));
        configured = typeIndex >= 0 && sourceIndex >= 0;
        if (!configured) { dialog->reject(); return; }
        type->setCurrentIndex(typeIndex);
        source->setCurrentIndex(sourceIndex);
        dialog->accept();
    });
    timer.start();
    const bool invoked = QMetaObject::invokeMethod(&window, "configureTrackMatte", Qt::DirectConnection);
    timer.stop();
    return invoked && configured;
}

} // namespace

int runMatteExportSelftest()
{
    selftest::GateTally tally;
    QString encoderReason;
    if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &encoderReason)) {
        const QByteArray reason = encoderReason.toUtf8();
        for (int gate = 1; gate <= 5; ++gate) tally.skip(gate, reason.constData());
        return tally.finish("matte-export");
    }

    QTemporaryDir temporary;
    const QString overridePath = qEnvironmentVariable("VEDITOR_MATTE_EXPORT_OUT");
    const QString outputDir = overridePath.isEmpty() ? temporary.path() : overridePath;
    const bool directoryReady = temporary.isValid() && QDir().mkpath(outputDir);
    const auto path = [&](const QString &name) { return QDir(outputDir).filePath(name); };
    const QString basePath = path(QStringLiteral("black-base.mp4"));
    const QString sourcePath = path(QStringLiteral("source.mp4"));
    const QString redPath = path(QStringLiteral("red.mp4"));
    QString fixtureError;
    const bool mediaReady = directoryReady
        && writeClip(basePath, Pattern::Black, &fixtureError)
        && writeClip(sourcePath, Pattern::HalfBlackWhite, &fixtureError)
        && writeClip(redPath, Pattern::Red, &fixtureError);

    // RM-3: V1 is the opaque base and cannot be a matte source. V2 provides
    // luma to V3; without a matte, V1 wins the ordinary stack and is black.
    const QVector<QVector<ClipInfo>> tracks{{clip(basePath)}, {clip(sourcePath)}, {clip(redPath)}};
    Timeline timeline;
    timeline.restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    timeline.setProjectOutputConfig(kWidth, kHeight, true, kFps);
    timeline.setTrackMatteEntries({{trackMatteClipKey(2, 0), lumaEntry()}});
    const QString livePath = path(QStringLiteral("live.mp4"));
    const ExportResult live = mediaReady
        ? exportQueue(jobFor(&timeline, {}, livePath)) : ExportResult{};
    const Halves liveHalves = measure(live.frame);
    tally.gate(1, mediaReady && live.done && live.success && live.sawJob
        && blackLeftRedRight(liveHalves), detail(liveHalves,
            QStringLiteral("done=%1 success=%2 sawJob=%3 error=%4 fixtureError=%5")
                .arg(live.done).arg(live.success).arg(live.sawJob)
                .arg(live.error, fixtureError)));

    timeline.setTrackMatteEntries({});
    const ExportResult noMatte = mediaReady
        ? exportQueue(jobFor(&timeline, {}, path(QStringLiteral("no-matte.mp4")))) : ExportResult{};
    const Halves noMatteHalves = measure(noMatte.frame);
    tally.gate(2, mediaReady && noMatte.done && noMatte.success && noMatte.sawJob
        && blackBoth(noMatteHalves), detail(noMatteHalves,
            QStringLiteral("done=%1 success=%2 sawJob=%3 error=%4")
                .arg(noMatte.done).arg(noMatte.success).arg(noMatte.sawJob).arg(noMatte.error)));
    timeline.setTrackMatteEntries({{trackMatteClipKey(2, 0), lumaEntry()}});

    ProjectData project;
    project.videoTracks = tracks;
    project.config.width = kWidth;
    project.config.height = kHeight;
    project.config.fps = kFps;
    project.config.explicitOutputResolution = true;
    TrackMatteClipEntry savedMatte;
    savedMatte.clipId = trackMatteClipKey(2, 0);
    savedMatte.matteType = TrackMatteType::LumaMatte;
    savedMatte.matteSourceClipId = trackMatteClipKey(1, 0);
    project.trackMatteClipEntries.append(savedMatte);
    const QString projectPath = path(QStringLiteral("matte.veditor"));
    const bool saved = mediaReady && ProjectFile::save(projectPath, project);
    const QString filePath = path(QStringLiteral("from-project.mp4"));
    const ExportResult fromFile = saved
        ? exportQueue(jobFor(nullptr, projectPath, filePath)) : ExportResult{};
    const Halves fileHalves = measure(fromFile.frame);
    const bool fileBytesEqual = equalFiles(livePath, filePath);
    tally.gate(3, saved && fromFile.done && fromFile.success && fromFile.sawJob
        && equalPixels(live.frame, fromFile.frame), detail(fileHalves,
            QStringLiteral("saved=%1 done=%2 success=%3 sawJob=%4 pixelEqual=%5 byteEqual=%6 error=%7")
                .arg(saved).arg(fromFile.done).arg(fromFile.success).arg(fromFile.sawJob)
                .arg(equalPixels(live.frame, fromFile.frame)).arg(fileBytesEqual).arg(fromFile.error)));

    const QString repeatPath = path(QStringLiteral("live-repeat.mp4"));
    const ExportResult repeated = mediaReady
        ? exportQueue(jobFor(&timeline, {}, repeatPath)) : ExportResult{};
    const Halves repeatHalves = measure(repeated.frame);
    const bool repeatBytesEqual = equalFiles(livePath, repeatPath);
    const QString repeatDetail = QStringLiteral(
        "done=%1 success=%2 sawJob=%3 pixelEqual=%4 byteEqual=%5 error=%6")
        .arg(repeated.done).arg(repeated.success).arg(repeated.sawJob)
        .arg(equalPixels(live.frame, repeated.frame)).arg(repeatBytesEqual)
        .arg(repeated.error);
    tally.gate(4, repeated.done && repeated.success && repeated.sawJob
        && equalPixels(live.frame, repeated.frame), detail(repeatHalves,
            repeatDetail + (repeatBytesEqual
                ? QStringLiteral(" promotion=byte-identical") : QString{})));

    const QString mcpPath = path(QStringLiteral("mcp.mp4"));
    bool serverReady = false, timelineReady = false, selected = false, configured = false;
    bool accepted = false, mcpDone = false, mcpSuccess = false;
    QString mcpError, jobId;
    QImage mcpFrame;
    if (mediaReady) {
        QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
        const QVariant priorAutoStart = settings.value(QStringLiteral("mcpAutoStart"));
        const bool priorTokenSet = qEnvironmentVariableIsSet("VEDITOR_MCP_TOKEN");
        const QByteArray priorToken = qgetenv("VEDITOR_MCP_TOKEN");
        qputenv("VEDITOR_MCP_TOKEN", "matte-export-selftest-token");
        QTimer serverWarningGuard;
        serverWarningGuard.setInterval(10);
        QObject::connect(&serverWarningGuard, &QTimer::timeout, [&] {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                message->done(QMessageBox::Ok);
        });
        serverWarningGuard.start();
        MainWindow window;
        window.hide();
        Timeline *tl = window.findChild<Timeline *>();
        timelineReady = tl != nullptr;
        if (tl) {
            tl->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
            tl->setProjectOutputConfig(kWidth, kHeight, true, kFps);
        }
        window.toggleMcpServer(true);
        serverWarningGuard.stop();
        auto *server = window.findChild<mcp::McpHttpServer *>();
        serverReady = server && server->isRunning();
        if (serverReady) {
            selected = timelineReady && toolOk(callMcp(server, QStringLiteral("select_clip"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("video")},
                    {QStringLiteral("trackIndex"), 2}, {QStringLiteral("clipIndex"), 0}}));
            configured = selected && configureMatte(window);
            if (configured) {
                const QJsonObject response = callMcp(server, QStringLiteral("export_video"),
                    QJsonObject{{QStringLiteral("outputPath"), mcpPath},
                        {QStringLiteral("width"), kWidth}, {QStringLiteral("height"), kHeight},
                        {QStringLiteral("fps"), kFps},
                        {QStringLiteral("videoCodec"), QStringLiteral("mpeg4")},
                        {QStringLiteral("videoBitrate"), 600}});
                accepted = !response.isEmpty()
                    && !response.value(QStringLiteral("isError")).toBool(true);
                jobId = response.value(QStringLiteral("structuredContent"))
                    .toObject().value(QStringLiteral("jobId")).toString();
                accepted &= !jobId.isEmpty();
                if (accepted) {
                    mcpDone = QTest::qWaitFor([&] {
                        const QJsonObject statusResult = callMcp(server,
                            QStringLiteral("get_export_status"),
                            QJsonObject{{QStringLiteral("jobId"), jobId}});
                        if (!toolOk(statusResult)) return false;
                        const QJsonObject status = statusResult.value(
                            QStringLiteral("structuredContent")).toObject();
                        const QString state = status.value(QStringLiteral("status")).toString();
                        mcpSuccess = state == QStringLiteral("done");
                        mcpError = status.value(QStringLiteral("error")).toString();
                        return mcpSuccess || state == QStringLiteral("failed");
                    }, 120000);
                    if (mcpDone && mcpSuccess)
                        mcpFrame = libavcore::grabFrameAt(mcpPath, 0.5);
                }
            }
        }
        window.toggleMcpServer(false);
        if (priorTokenSet) qputenv("VEDITOR_MCP_TOKEN", priorToken);
        else qunsetenv("VEDITOR_MCP_TOKEN");
        if (priorAutoStart.isValid()) settings.setValue(QStringLiteral("mcpAutoStart"), priorAutoStart);
        else settings.remove(QStringLiteral("mcpAutoStart"));
        settings.sync();
    }
    const Halves mcpHalves = measure(mcpFrame);
    tally.gate(5, mediaReady && timelineReady && serverReady && selected && configured
        && accepted && mcpDone && mcpSuccess && blackLeftRedRight(mcpHalves),
        detail(mcpHalves,
            QStringLiteral("mediaReady=%1 timeline=%2 server=%3 selected=%4 configured=%5 "
                           "accepted=%6 jobId=%7 done=%8 success=%9 error=%10")
                .arg(mediaReady).arg(timelineReady).arg(serverReady).arg(selected).arg(configured)
                .arg(accepted).arg(jobId).arg(mcpDone).arg(mcpSuccess).arg(mcpError)));

    return tally.finish("matte-export");
}
