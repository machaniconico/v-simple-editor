#include "RenderQueue.h"
#include "TimelineFrameRenderer.h"
#include "ProjectFile.h"
#include "Timeline.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>
#include <QThread>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QtGlobal>
#include <cmath>  // S11: std::round for HDR10 master-display luminance

namespace {
QString makeUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace

RenderQueue::RenderQueue(QObject *parent)
    : QObject(parent)
{
}

RenderQueue::~RenderQueue()
{
    m_cancelRequested = true;
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    if (m_renderThread) {
        m_renderThread->wait(10000);
        delete m_renderThread;
        m_renderThread = nullptr;
    }
    if (m_process) {
        delete m_process;
        m_process = nullptr;
    }
}

int RenderQueue::addJob(const QString &name, const QString &projectFilePath,
                        const QString &outputPath, const QJsonObject &exportConfig)
{
    RenderJob job;
    job.id = m_nextId++;
    job.uuid = makeUuid();
    job.name = name;
    job.projectFilePath = projectFilePath;
    job.outputPath = outputPath;
    job.exportConfig = exportConfig;
    job.status = RenderJobStatus::Pending;
    job.progress = 0;
    job.progressPercent = 0;

    // Mirror legacy exportConfig into flat fields so spec API can read them.
    job.width = exportConfig.value("width").toInt(1920);
    job.height = exportConfig.value("height").toInt(1080);
    job.codec = exportConfig.value("videoCodec").toString("h264");
    // exportConfig stored bitrate in kbps; flat field uses bps.
    job.bitrateBps = exportConfig.value("videoBitrate").toInt(10000) * 1000;
    job.preset = exportConfig.value("preset").toString();

    m_jobs.append(job);
    emit jobsChanged();
    return job.id;
}

void RenderQueue::addJob(const RenderJob &job)
{
    RenderJob copy = job;
    if (copy.uuid.isEmpty())
        copy.uuid = makeUuid();
    if (copy.id == 0)
        copy.id = m_nextId++;
    if (copy.status == RenderJobStatus::Pending && copy.progress == 0)
        copy.progress = copy.progressPercent;
    if (copy.errorMessage.isEmpty())
        copy.errorMessage = copy.error;

    // Mirror flat fields into exportConfig so the FFmpeg arg builder picks
    // them up without a separate code path.
    QJsonObject cfg = copy.exportConfig;
    cfg["width"] = copy.width;
    cfg["height"] = copy.height;
    cfg["videoCodec"] = mapCodecToFFmpeg(copy.codec);
    cfg["videoBitrate"] = copy.bitrateBps / 1000;  // ffmpeg arg builder expects kbps
    if (!copy.preset.isEmpty())
        cfg["preset"] = copy.preset;
    copy.exportConfig = cfg;

    m_jobs.append(copy);
    emit jobsChanged();
}

void RenderQueue::removeJob(int id)
{
    int idx = findJobIndex(id);
    if (idx < 0)
        return;

    // Don't remove the currently rendering job
    if (m_jobs[idx].status == RenderJobStatus::Rendering)
        return;

    m_jobs.removeAt(idx);

    // Adjust current index if needed
    if (m_currentJobIndex > idx)
        m_currentJobIndex--;
    else if (m_currentJobIndex == idx)
        m_currentJobIndex = -1;

    emit jobsChanged();
}

void RenderQueue::removeJob(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx < 0)
        return;
    if (m_jobs[idx].status == RenderJobStatus::Rendering)
        return;

    m_jobs.removeAt(idx);
    if (m_currentJobIndex > idx)
        m_currentJobIndex--;
    else if (m_currentJobIndex == idx)
        m_currentJobIndex = -1;

    emit jobsChanged();
}

void RenderQueue::clear()
{
    clearAll();
}

void RenderQueue::start()
{
    startQueue();
}

void RenderQueue::stop()
{
    cancelCurrent();
    m_running = false;
    m_paused = false;
}

void RenderQueue::clearCompleted()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].status == RenderJobStatus::Completed ||
            m_jobs[i].status == RenderJobStatus::Failed ||
            m_jobs[i].status == RenderJobStatus::Cancelled) {
            m_jobs.removeAt(i);
            if (m_currentJobIndex > i)
                m_currentJobIndex--;
        }
    }
    emit jobsChanged();
}

void RenderQueue::clearAll()
{
    if (m_running)
        cancelCurrent();

    m_jobs.clear();
    m_currentJobIndex = -1;
    emit jobsChanged();
}

void RenderQueue::startQueue()
{
    if (m_running)
        return;

    m_running = true;
    m_paused = false;
    startNextJob();
}

void RenderQueue::pauseQueue()
{
    m_paused = true;
    // Current job keeps running, but no new jobs will start
}

void RenderQueue::resumeQueue()
{
    if (!m_paused)
        return;

    m_paused = false;

    // If no job is currently rendering, start the next one
    if (m_currentJobIndex < 0 || m_jobs[m_currentJobIndex].status != RenderJobStatus::Rendering)
        startNextJob();
}

void RenderQueue::cancelCurrent()
{
    if (m_currentJobIndex < 0)
        return;

    // Signal the S8 render-pipe worker (if running) to stop streaming frames;
    // it polls m_cancelRequested between frames exactly like
    // VideoStabilizer::m_cancelled.
    m_cancelRequested = true;

    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }

    RenderJob &j = m_jobs[m_currentJobIndex];
    j.status = RenderJobStatus::Cancelled;
    j.endTime = QDateTime::currentDateTime();
    j.errorMessage = "Cancelled by user";
    j.error = j.errorMessage;

    // Don't auto-advance; let the render-pipe completion handler deal with it
}

const RenderJob *RenderQueue::currentJob() const
{
    if (m_currentJobIndex >= 0 && m_currentJobIndex < m_jobs.size() &&
        m_jobs[m_currentJobIndex].status == RenderJobStatus::Rendering) {
        return &m_jobs[m_currentJobIndex];
    }
    return nullptr;
}

int RenderQueue::pendingCount() const
{
    int count = 0;
    for (const auto &job : m_jobs) {
        if (job.status == RenderJobStatus::Pending)
            count++;
    }
    return count;
}

int RenderQueue::completedCount() const
{
    int count = 0;
    for (const auto &job : m_jobs) {
        if (job.status == RenderJobStatus::Completed)
            count++;
    }
    return count;
}

void RenderQueue::moveJobUp(int id)
{
    int idx = findJobIndex(id);
    if (idx <= 0)
        return;

    // Only reorder pending jobs
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;

    m_jobs.swapItemsAt(idx, idx - 1);
    emit jobsChanged();
}

void RenderQueue::moveJobDown(int id)
{
    int idx = findJobIndex(id);
    if (idx < 0 || idx >= m_jobs.size() - 1)
        return;

    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;

    m_jobs.swapItemsAt(idx, idx + 1);
    emit jobsChanged();
}

void RenderQueue::moveJobUpUuid(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx <= 0)
        return;
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;
    m_jobs.swapItemsAt(idx, idx - 1);
    emit jobsChanged();
}

void RenderQueue::moveJobDownUuid(const QString &uuid)
{
    int idx = findJobIndexByUuid(uuid);
    if (idx < 0 || idx >= m_jobs.size() - 1)
        return;
    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;
    m_jobs.swapItemsAt(idx, idx + 1);
    emit jobsChanged();
}

int RenderQueue::totalEstimatedTime() const
{
    int totalSeconds = 0;
    for (const auto &job : m_jobs) {
        if (job.status != RenderJobStatus::Pending)
            continue;

        // Rough estimate: file size in MB * 2 seconds per MB
        QFileInfo info(job.projectFilePath);
        if (info.exists()) {
            int sizeMB = static_cast<int>(info.size() / (1024 * 1024));
            totalSeconds += qMax(sizeMB * 2, 10);  // minimum 10 seconds per job
        } else {
            totalSeconds += 30;  // default estimate
        }
    }
    return totalSeconds;
}

bool RenderQueue::saveQueue(const QString &filePath) const
{
    QJsonArray jobsArray;
    for (const auto &job : m_jobs) {
        QJsonObject obj;
        obj["id"] = job.id;                 // legacy int id
        obj["uuid"] = job.uuid;             // spec QString uuid
        obj["name"] = job.name;
        obj["projectFilePath"] = job.projectFilePath;
        obj["outputPath"] = job.outputPath;
        obj["preset"] = job.preset;
        obj["width"] = job.width;
        obj["height"] = job.height;
        obj["codec"] = job.codec;
        obj["bitrateBps"] = job.bitrateBps;
        obj["startUs"] = static_cast<qint64>(job.startUs);
        obj["endUs"] = static_cast<qint64>(job.endUs);
        obj["passes"] = job.passes;
        obj["exportConfig"] = job.exportConfig;
        obj["status"] = static_cast<int>(job.status);
        obj["statusString"] = job.statusString();
        obj["progressPercent"] = job.progressPercent;
        obj["progress"] = job.progress;
        obj["error"] = job.error;
        obj["errorMessage"] = job.errorMessage;

        if (job.startTime.isValid())
            obj["startTime"] = job.startTime.toString(Qt::ISODate);
        if (job.endTime.isValid())
            obj["endTime"] = job.endTime.toString(Qt::ISODate);

        jobsArray.append(obj);
    }

    QJsonObject root;
    root["version"] = 2;
    root["nextId"] = m_nextId;
    root["jobs"] = jobsArray;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(QJsonDocument(root).toJson());
    return true;
}

bool RenderQueue::loadQueue(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    QJsonObject root = doc.object();
    m_nextId = root["nextId"].toInt(1);

    m_jobs.clear();
    QJsonArray jobsArray = root["jobs"].toArray();
    for (const QJsonValue &val : jobsArray) {
        QJsonObject obj = val.toObject();
        RenderJob job;
        job.id = obj["id"].toInt();
        job.uuid = obj.value("uuid").toString(makeUuid());
        job.name = obj["name"].toString();
        job.projectFilePath = obj["projectFilePath"].toString();
        job.outputPath = obj["outputPath"].toString();
        job.preset = obj["preset"].toString();
        job.width = obj["width"].toInt(1920);
        job.height = obj["height"].toInt(1080);
        job.codec = obj["codec"].toString("h264");
        job.bitrateBps = obj["bitrateBps"].toInt(50000000);
        job.startUs = static_cast<qint64>(obj["startUs"].toDouble(0));
        job.endUs = static_cast<qint64>(obj["endUs"].toDouble(0));
        job.passes = obj["passes"].toInt(1);
        job.exportConfig = obj["exportConfig"].toObject();
        job.status = static_cast<RenderJobStatus>(obj["status"].toInt());
        job.progressPercent = obj["progressPercent"].toInt(obj["progress"].toInt());
        job.progress = job.progressPercent;
        job.error = obj["error"].toString(obj["errorMessage"].toString());
        job.errorMessage = job.error;

        if (obj.contains("startTime"))
            job.startTime = QDateTime::fromString(obj["startTime"].toString(), Qt::ISODate);
        if (obj.contains("endTime"))
            job.endTime = QDateTime::fromString(obj["endTime"].toString(), Qt::ISODate);

        m_jobs.append(job);
    }

    emit jobsChanged();
    return true;
}

void RenderQueue::startNextJob()
{
    if (m_paused) {
        m_running = false;
        return;
    }

    // Find next pending job
    m_currentJobIndex = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == RenderJobStatus::Pending) {
            m_currentJobIndex = i;
            break;
        }
    }

    if (m_currentJobIndex < 0) {
        m_running = false;
        emit queueFinished();
        emit allCompleted();
        return;
    }

    RenderJob &job = m_jobs[m_currentJobIndex];
    job.status = RenderJobStatus::Rendering;
    job.progress = 0;
    job.progressPercent = 0;
    job.startTime = QDateTime::currentDateTime();
    m_currentDuration = 0.0;
    m_cancelRequested = false;

    emit jobStarted(job.id);
    emit jobsChanged();

    // S8: the export is no longer a source-file passthrough transcode. Drive
    // the SSOT render-pipe — every output frame comes from
    // tlrender::renderFrameAt so the entire edit graph (grade / FX / masks /
    // tracking / text / multi-track) reaches the exported file, with the
    // original audio muxed in. The public RenderJob/signal/queue-advance
    // contract is unchanged: jobProgress per frame, jobCompleted / jobFailed
    // on finish, cancellation honoured, and the next pending job is started
    // from finishCurrentJob().
    startRenderPipe(m_currentJobIndex);
}

// S8: synchronous frame-render + ffmpeg-stdin streaming on a worker thread.
// Mirrors the proven VideoStabilizer::stabilizePlanarInversion pipe
// (VideoStabilizer.cpp:359-516): an ffmpeg "-f rawvideo -pix_fmt rgba ..."
// encoder fed RGBA frames over stdin, with the original media muxed back in
// as a second input. Here the frames come from tlrender::renderFrameAt
// instead of a decoded source, which is the whole point of the story.
void RenderQueue::startRenderPipe(int jobIndex)
{
    const RenderJob jobCopy = m_jobs[jobIndex];

    if (m_process) {
        delete m_process;
        m_process = nullptr;
    }
    if (m_renderThread) {
        m_renderThread->wait(5000);
        delete m_renderThread;
        m_renderThread = nullptr;
    }

    // ── Resolve everything that touches the GUI/Timeline on THIS thread ─────
    // Timeline is a QWidget; constructing / restoring it must happen on the
    // GUI thread (the parity selftest + MainWindow both create it there). The
    // worker thread below only calls tlrender::renderFrameAt + QProcess I/O.
    // renderFrameAt is pure libav decode + CPU QPainter/QImage compositing
    // with NO QWidget construction — including its S6 text-overlay stage,
    // which bakes through the FREE function textbake::bakeOverlays (NOT a
    // VideoPlayer). Constructing a QWidget off the GUI thread is Qt undefined
    // behaviour, so this property is load-bearing for any timeline that
    // carries a text overlay. So we resolve the Timeline, geometry, frame
    // count and audio input path here and hand plain values to the worker.
    const QString ffmpegBin = findFFmpegBinary();
    if (ffmpegBin.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            finishCurrentJob(false,
                              QStringLiteral("ffmpeg binary not found"));
        }, Qt::QueuedConnection);
        return;
    }

    Timeline *owned = nullptr;
    Timeline *tl = resolveTimeline(jobCopy, &owned);
    if (!tl) {
        delete owned;
        QMetaObject::invokeMethod(this, [this, jobCopy]() {
            finishCurrentJob(false, QStringLiteral(
                "could not obtain a Timeline for job (project load failed and "
                "no in-memory timeline supplied): ") + jobCopy.projectFilePath);
        }, Qt::QueuedConnection);
        return;
    }

    const QJsonObject &cfg = jobCopy.exportConfig;
    int outW = cfg.value("width").toInt(jobCopy.width > 0 ? jobCopy.width : 1920);
    int outH = cfg.value("height").toInt(jobCopy.height > 0 ? jobCopy.height : 1080);
    if (outW <= 0) outW = 1920;
    if (outH <= 0) outH = 1080;
    // ffmpeg rejects odd dimensions for yuv420p H.264; round down to even.
    outW &= ~1;
    outH &= ~1;
    double fps = cfg.value("fps").toDouble(0.0);
    if (fps <= 0.0)
        fps = 30.0;

    // Iterate from 0 to the timeline duration at the project fps.
    // usec-per-frame = 1e6 / fps (the renderFrameAt time unit).
    const double durationSec = tl->totalDuration();
    qint64 totalFrames = static_cast<qint64>(durationSec * fps + 0.5);
    if (jobCopy.endUs > 0 && jobCopy.endUs > jobCopy.startUs) {
        const double rangeSec =
            static_cast<double>(jobCopy.endUs - jobCopy.startUs) / 1'000'000.0;
        totalFrames = static_cast<qint64>(rangeSec * fps + 0.5);
    }
    if (totalFrames <= 0)
        totalFrames = 1;
    const qint64 startUsec = jobCopy.startUs > 0 ? jobCopy.startUs : 0;
    const double usecPerFrame = 1'000'000.0 / fps;

    // Audio mux input: the original source file's audio stream. Mirror
    // VideoStabilizer.cpp:405-408 (-i original, -map 1:a?, -c:a aac). The
    // job's projectFilePath is the source media for legacy/transcode callers;
    // when the in-memory timeline seam is used (projectFilePath is a .veditor
    // or empty), fall back to the V1 clip's source file so the timeline audio
    // still rides along.
    QString audioInputPath = jobCopy.projectFilePath;
    {
        const bool projIsMedia = !audioInputPath.isEmpty()
            && QFile::exists(audioInputPath)
            && !audioInputPath.endsWith(QStringLiteral(".veditor"),
                                        Qt::CaseInsensitive);
        if (!projIsMedia) {
            const QVector<ClipInfo> &v1 = tl->videoClips();
            if (!v1.isEmpty() && QFile::exists(v1.first().filePath))
                audioInputPath = v1.first().filePath;
            else
                audioInputPath.clear();
        }
    }

    const QStringList args =
        buildRenderPipeArgs(jobCopy, outW, outH, fps, audioInputPath);

    m_renderThread = QThread::create(
        [this, jobCopy, tl, owned, ffmpegBin, args, outW, outH,
         totalFrames, startUsec, usecPerFrame]() {
        QString failMsg;
        // Delete the heap Timeline (only set when loaded from a project
        // file). It is a QWidget created on the GUI thread; QObject deletion
        // is safe from another thread only if it has no thread affinity ops
        // pending — Timeline here is render-only (never shown, no event loop
        // posted to it), so deleteLater on the owning thread is the safe
        // disposal. We schedule that back on the queue thread at the end.
        const bool ok = [&]() -> bool {
            QProcess *proc = new QProcess();
            proc->setProcessChannelMode(QProcess::SeparateChannels);
            m_process = proc;
            proc->start(ffmpegBin, args);
            if (!proc->waitForStarted(15000)) {
                failMsg = QStringLiteral("failed to start ffmpeg: ")
                    + proc->errorString();
                m_process = nullptr;
                proc->deleteLater();
                return false;
            }

            const QSize outSize(outW, outH);
            int lastPct = -1;
            for (qint64 f = 0; f < totalFrames; ++f) {
                if (m_cancelRequested) {
                    proc->kill();
                    proc->waitForFinished(3000);
                    m_process = nullptr;
                    proc->deleteLater();
                    failMsg = QStringLiteral("cancelled");
                    return false;
                }

                const qint64 usec =
                    startUsec + static_cast<qint64>(f * usecPerFrame);
                QImage frame = tlrender::renderFrameAt(tl, usec, outSize);
                if (frame.isNull()) {
                    proc->kill();
                    proc->waitForFinished(3000);
                    m_process = nullptr;
                    proc->deleteLater();
                    failMsg = QStringLiteral(
                        "renderFrameAt returned a null image at frame ")
                        + QString::number(f);
                    return false;
                }
                if (frame.format() != QImage::Format_RGBA8888)
                    frame = frame.convertToFormat(QImage::Format_RGBA8888);
                if (frame.width() != outW || frame.height() != outH)
                    frame = frame.scaled(outSize, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation)
                                .convertToFormat(QImage::Format_RGBA8888);

                // A video file is OPAQUE — it has no alpha channel. The SSOT
                // frame can be partially transparent (a per-clip mask with no
                // lower track leaves alpha=0 regions). The genuine preview
                // displays that composite over a BLACK canvas
                // (VideoPlayer ARGB32_Premultiplied base, VideoPlayer.cpp:1939
                // fills black) and the genuine Exporter encodes through an
                // opaque Format_RGB888 (Exporter.cpp:482/499, AV_PIX_FMT_RGB24)
                // which carries no alpha. So the correct deliverable is the
                // SSOT flattened onto opaque black. Compose onto a black
                // Format_RGB888 and feed ffmpeg rgb24 (3 bytes/px, no alpha) —
                // identical pixel semantics to the Exporter's RGB24 path.
                QImage rgb(outW, outH, QImage::Format_RGB888);
                rgb.fill(Qt::black);
                {
                    QPainter pp(&rgb);
                    pp.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    pp.drawImage(0, 0, frame);
                }

                // ffmpeg -s WxH -pix_fmt rgb24 expects exactly W*H*3 bytes per
                // frame with NO row padding; Qt pads Format_RGB888 scanlines
                // to a 4-byte boundary, so write row by row.
                const int rgbRowBytes = outW * 3;
                for (int y = 0; y < outH; ++y) {
                    const char *row =
                        reinterpret_cast<const char *>(rgb.constScanLine(y));
                    qint64 rem = rgbRowBytes;
                    while (rem > 0) {
                        const qint64 wrote = proc->write(row, rem);
                        if (wrote < 0) {
                            proc->kill();
                            proc->waitForFinished(3000);
                            m_process = nullptr;
                            proc->deleteLater();
                            failMsg =
                                QStringLiteral("ffmpeg stdin write failed");
                            return false;
                        }
                        row += wrote;
                        rem -= wrote;
                        if (rem > 0 && !proc->waitForBytesWritten(15000)) {
                            proc->kill();
                            proc->waitForFinished(3000);
                            m_process = nullptr;
                            proc->deleteLater();
                            failMsg = QStringLiteral("ffmpeg stdin stalled");
                            return false;
                        }
                    }
                }

                const int pct = qBound(0, static_cast<int>(
                    (static_cast<double>(f + 1)
                        / static_cast<double>(totalFrames)) * 100.0), 99);
                if (pct != lastPct) {
                    lastPct = pct;
                    emit jobProgress(jobCopy.id, pct);
                    emit jobProgressUuid(jobCopy.uuid, pct);
                }
            }

            proc->closeWriteChannel();
            proc->waitForFinished(-1);
            const bool encOk = proc->exitStatus() == QProcess::NormalExit
                && proc->exitCode() == 0;
            if (!encOk)
                failMsg = QStringLiteral("ffmpeg exited with code ")
                    + QString::number(proc->exitCode());
            m_process = nullptr;
            proc->deleteLater();
            return encOk;
        }();

        // Dispose the heap Timeline (render-only QWidget, never shown). Hand
        // the result back to the queue thread; delete owned there so the
        // QWidget is destroyed on the GUI/queue thread that created it.
        QMetaObject::invokeMethod(this, [this, ok, failMsg, owned]() {
            delete owned;
            finishCurrentJob(ok, failMsg);
        }, Qt::QueuedConnection);
    });
    m_renderThread->start();
}

// Finalise the current job and advance the queue. Always runs on the
// RenderQueue's (main/queue) thread — preserves the exact public contract the
// old QProcess::finished lambda had.
void RenderQueue::finishCurrentJob(bool success, const QString &errorMsg)
{
    if (m_renderThread) {
        m_renderThread->wait(5000);
        m_renderThread->deleteLater();
        m_renderThread = nullptr;
    }

    if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
        return;

    RenderJob &job = m_jobs[m_currentJobIndex];
    job.endTime = QDateTime::currentDateTime();

    if (job.status == RenderJobStatus::Cancelled || m_cancelRequested) {
        job.status = RenderJobStatus::Cancelled;
        if (job.errorMessage.isEmpty()) {
            job.errorMessage = QStringLiteral("Cancelled by user");
            job.error = job.errorMessage;
        }
        emit jobCompletedUuid(job.uuid, false, job.error);
    } else if (!success) {
        job.status = RenderJobStatus::Failed;
        job.errorMessage = errorMsg.isEmpty()
            ? QStringLiteral("Render pipe failed") : errorMsg;
        job.error = job.errorMessage;
        emit jobFailed(job.id, job.errorMessage);
        emit jobCompletedUuid(job.uuid, false, job.errorMessage);
    } else {
        job.status = RenderJobStatus::Completed;
        job.progress = 100;
        job.progressPercent = 100;
        emit jobProgress(job.id, 100);
        emit jobProgressUuid(job.uuid, 100);
        emit jobCompleted(job.id);
        emit jobCompletedUuid(job.uuid, true, QString());
    }

    emit jobsChanged();

    // Emit overall queue progress (unchanged from the legacy handler).
    int total = m_jobs.size();
    int done = completedCount();
    for (const auto &j : m_jobs) {
        if (j.status == RenderJobStatus::Failed
            || j.status == RenderJobStatus::Cancelled)
            done++;
    }
    if (total > 0)
        emit queueProgress(done * 100 / total);

    m_cancelRequested = false;

    // Advance to the next pending job.
    startNextJob();
}

QStringList RenderQueue::buildFFmpegArgs(const RenderJob &job) const
{
    QStringList args;
    args << "-y";  // overwrite output

    const QJsonObject &cfg = job.exportConfig;

    // Optional timeline range trim (flat-field jobs only — legacy callers
    // leave both at 0 and the trim is skipped).
    if (job.startUs > 0) {
        args << "-ss" << QString::number(job.startUs / 1000000.0, 'f', 6);
    }
    if (job.endUs > 0 && job.endUs > job.startUs) {
        args << "-to" << QString::number(job.endUs / 1000000.0, 'f', 6);
    }

    // Input file — use project file path as source
    args << "-i" << job.projectFilePath;

    // Video codec
    QString videoCodec = cfg["videoCodec"].toString("libx264");
    args << "-c:v" << videoCodec;

    // ProRes uses -profile:v rather than a bitrate flag.
    const bool isProRes = (videoCodec == "prores_ks" || videoCodec == "prores");
    if (isProRes) {
        // Default to ProRes 422 LT (profile 1). buildFFmpegArgs is const so
        // we read profile from cfg / flat-field codec only.
        int profile = cfg.value("proresProfile").toInt(1);
        args << "-profile:v" << QString::number(profile);
    } else {
        // Video bitrate (kbps in cfg)
        int videoBitrate = cfg["videoBitrate"].toInt(10000);
        args << "-b:v" << QString("%1k").arg(videoBitrate);
    }

    // Resolution
    int width = cfg["width"].toInt(1920);
    int height = cfg["height"].toInt(1080);
    args << "-vf" << QString("scale=%1:%2").arg(width).arg(height);

    // Frame rate
    int fps = cfg["fps"].toInt(30);
    args << "-r" << QString::number(fps);

    // Audio codec
    QString audioCodec = cfg["audioCodec"].toString("aac");
    args << "-c:a" << audioCodec;

    // Audio bitrate
    int audioBitrate = cfg["audioBitrate"].toInt(192);
    args << "-b:a" << QString("%1k").arg(audioBitrate);

    // Codec-specific options
    if (videoCodec == "libx264" || videoCodec == "libx265") {
        args << "-preset" << "medium";
    } else if (videoCodec == "libsvtav1") {
        args << "-preset" << "8";
    } else if (videoCodec == "libvpx-vp9") {
        args << "-quality" << "good" << "-cpu-used" << "4";
    }

    // 2-pass VBR support — when job.passes==2 the first pass writes to a
    // null muxer, the second pass is what produces the final file. Here we
    // just hint the encoder; orchestrating both runs is left to the caller
    // / future work (the legacy single-pass path stays the default).
    if (job.passes == 2 && !isProRes) {
        args << "-pass" << "2";
    }

    // Progress reporting
    args << "-progress" << "pipe:1";

    // Output
    args << job.outputPath;

    return args;
}

// S8: ffmpeg argv for the SSOT render-pipe. Video comes from renderFrameAt
// over stdin as packed RGBA (input 0); the original media is the second input
// purely so its audio stream can be muxed into the output. This is the exact
// rawvideo + second-input pattern proven in VideoStabilizer.cpp:397-410:
//   ffmpeg -y -f rawvideo -pix_fmt rgba -s WxH -r FPS -i -
//          -i <originalMedia> -map 0:v:0 -map 1:a? -c:a aac ... output
QStringList RenderQueue::buildRenderPipeArgs(const RenderJob &job,
                                             int outW, int outH, double fps,
                                             const QString &audioInputPath) const
{
    const QJsonObject &cfg = job.exportConfig;

    QString videoCodec = cfg.value("videoCodec").toString();
    if (videoCodec.isEmpty())
        videoCodec = mapCodecToFFmpeg(job.codec.isEmpty() ? QStringLiteral("h264")
                                                          : job.codec);

    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-hide_banner")
         // Input 0: the rendered frame stream on stdin. We flatten the SSOT
         // RGBA onto opaque black and feed rgb24 (3 bytes/px, NO alpha) so the
         // pixel semantics match the genuine Exporter's AV_PIX_FMT_RGB24 path
         // (Exporter.cpp:482-508) — a video file is opaque, so a per-clip
         // mask's transparent region correctly delivers as black.
         << QStringLiteral("-f") << QStringLiteral("rawvideo")
         << QStringLiteral("-pix_fmt") << QStringLiteral("rgb24")
         << QStringLiteral("-s:v")
         << QStringLiteral("%1x%2").arg(outW).arg(outH)
         << QStringLiteral("-r") << QString::number(fps, 'f', 6)
         << QStringLiteral("-i") << QStringLiteral("-");

    // Input 1: the original media — used ONLY for its audio stream
    // (VideoStabilizer.cpp:405-408 reuses the source the same way).
    const bool haveAudio = !audioInputPath.isEmpty()
        && QFile::exists(audioInputPath);
    if (haveAudio)
        args << QStringLiteral("-i") << audioInputPath;

    // Map the rendered video; map the source audio if present (optional so a
    // silent / video-only source still produces a valid file).
    args << QStringLiteral("-map") << QStringLiteral("0:v:0");
    if (haveAudio)
        args << QStringLiteral("-map") << QStringLiteral("1:a?");

    // ── S11: 10-bit / HDR10 / HLG / ProRes path ─────────────────────────────
    // DEFECT THIS FIXES (Exporter.cpp:471-474): the legacy Exporter's
    // `tenBitPath = isHdr10Mode || isHlgMode || proresProfile>=0` branch
    // sets `hasEffects = !tenBitPath && ...`, i.e. it BYPASSES the entire
    // effect graph for every 10-bit/HDR/ProRes output — an HDR10 clip with
    // a LUT exported WITHOUT the LUT. Routing the export through the SSOT
    // render-pipe (renderFrameAt feeds every frame, graph already applied)
    // and then ENCODING into the genuine 10-bit container closes that gap:
    // the grade/FX/mask/LUT now reach the 10-bit deliverable.
    //
    // PRECISION SCOPE (documented honestly): tlrender::renderFrameAt's
    // public contract is an 8-bit Format_RGBA8888 composite
    // (TimelineFrameRenderer.cpp:719-723); the whole CPU compositor
    // (applyColorCorrection / applyLut / applyEffectStack) emits
    // Format_RGB888. True >8-bit *internal* precision would require
    // rewriting the SSOT compositor and is OUT OF SCOPE. S11 delivers the
    // 8-bit composite (graph fully applied) lifted into a real 10-bit
    // container with correct BT.2020/PQ|HLG signalling — which is exactly
    // what removes the "LUT silently dropped for HDR" defect. The pixel
    // ladder is 8-bit-quantised but the container, bit-depth and colour
    // metadata are genuine 10-bit/HDR (so HDR-aware players tone-map it
    // correctly), and crucially the graph is no longer bypassed.
    //
    // The HDR encoder settings MIRROR the genuine Exporter
    // (Exporter.cpp:235-256, 271-287): HDR ⇒ libx265 main10 yuv420p10le
    // BT.2020 + SMPTE-2084 (HDR10) / ARIB-STD-B67 (HLG), limited range;
    // ProRes ⇒ yuv422p10le (profile<4) / yuva444p10le (profile>=4).
    const bool isProRes = (videoCodec == QLatin1String("prores_ks")
                           || videoCodec == QLatin1String("prores"));
    const QString hdrMode = cfg.value("hdrMode").toString().toLower();
    const bool isHdr10 = !isProRes
        && (hdrMode == QLatin1String("hdr10") || cfg.value("hdr10").toBool());
    const bool isHlg = !isProRes && hdrMode == QLatin1String("hlg");
    const int proresProfile = cfg.value("proresProfile").toInt(1);

    // HDR10/HLG are 10-bit and the genuine Exporter forces libx265 for them
    // (Exporter.cpp:271-287 — only the libx265 branch sets main10 + the HDR
    // x265-params). Override an 8-bit codec request so the container is
    // actually a valid HDR10/HLG stream.
    if ((isHdr10 || isHlg)
        && videoCodec != QLatin1String("libx265")
        && videoCodec != QLatin1String("hevc_nvenc")
        && videoCodec != QLatin1String("hevc_qsv")
        && videoCodec != QLatin1String("hevc_amf")) {
        videoCodec = QStringLiteral("libx265");
    }

    // Video codec + bitrate (mirrors buildFFmpegArgs' encoder selection).
    args << QStringLiteral("-c:v") << videoCodec;
    if (isProRes) {
        args << QStringLiteral("-profile:v")
             << QString::number(proresProfile);
    } else {
        int videoBitrateKbps = cfg.value("videoBitrate").toInt(0);
        if (videoBitrateKbps <= 0)
            videoBitrateKbps = job.bitrateBps > 0 ? job.bitrateBps / 1000
                                                  : 10000;
        args << QStringLiteral("-b:v")
             << QStringLiteral("%1k").arg(videoBitrateKbps);
    }

    // Pixel format: 10-bit for HDR/ProRes (mirror Exporter.cpp:235-242),
    // yuv420p otherwise so the 8-bit H.264/H.265 output stays broadly
    // playable (rawvideo rgb24 would otherwise leave the encoder in rgb).
    if (isProRes) {
        // Exporter.cpp:236-239: profile 4/5 (4444 / 4444 XQ) ⇒ 4:4:4 + alpha.
        args << QStringLiteral("-pix_fmt")
             << (proresProfile >= 4 ? QStringLiteral("yuva444p10le")
                                    : QStringLiteral("yuv422p10le"));
    } else if (isHdr10 || isHlg) {
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p10le");
        // Colour signalling — Exporter.cpp:246-256 (HDR10 = PQ/SMPTE-2084,
        // HLG = ARIB-STD-B67), BT.2020 primaries + non-constant luminance,
        // limited (tv) range. Carried in the bitstream so HDR-aware players
        // tone-map correctly.
        args << QStringLiteral("-color_primaries") << QStringLiteral("bt2020")
             << QStringLiteral("-colorspace") << QStringLiteral("bt2020nc")
             << QStringLiteral("-color_range") << QStringLiteral("tv")
             << QStringLiteral("-color_trc")
             << (isHdr10 ? QStringLiteral("smpte2084")
                         : QStringLiteral("arib-std-b67"));
    } else {
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }

    if (videoCodec == QLatin1String("libx264")
        || videoCodec == QLatin1String("libx265")) {
        args << QStringLiteral("-preset") << QStringLiteral("medium");
    } else if (videoCodec == QLatin1String("libsvtav1")) {
        args << QStringLiteral("-preset") << QStringLiteral("8");
    } else if (videoCodec == QLatin1String("libvpx-vp9")) {
        args << QStringLiteral("-quality") << QStringLiteral("good")
             << QStringLiteral("-cpu-used") << QStringLiteral("4");
    }

    // S11: libx265 10-bit HDR profile + x265-params — byte-for-byte the
    // genuine Exporter's HDR10/HLG x265 setup (Exporter.cpp:275-286 +
    // buildX265Hdr10Params Exporter.cpp:19-36). HDR10 master-display /
    // MaxCLL / MaxFALL come from exportConfig with the standard 1000-nit
    // P3-D65-in-2020 mastering defaults (the HDRSettings struct defaults).
    if (videoCodec == QLatin1String("libx265") && (isHdr10 || isHlg)) {
        args << QStringLiteral("-profile:v") << QStringLiteral("main10");
        if (isHdr10) {
            const double maxLum =
                cfg.value("hdrMasterMaxLum").toDouble(1000.0);
            const double minLum =
                cfg.value("hdrMasterMinLum").toDouble(0.0001);
            const int maxCll  = cfg.value("hdrMaxCll").toInt(1000);
            const int maxFall = cfg.value("hdrMaxFall").toInt(400);
            const int maxLumX10000 =
                static_cast<int>(std::round(maxLum * 10000.0));
            const int minLumX10000 =
                static_cast<int>(std::round(minLum * 10000.0));
            // Identical token order/content to buildX265Hdr10Params().
            const QString x265p =
                QStringLiteral(
                    "hdr10=1:repeat-headers=1:colorprim=bt2020:"
                    "transfer=smpte2084:colormatrix=bt2020nc:range=limited:"
                    "master-display=G(8500,39850)B(6550,2300)R(35400,14600)"
                    "WP(15635,16450)L(%1,%2):max-cll=%3,%4")
                    .arg(maxLumX10000)
                    .arg(minLumX10000)
                    .arg(maxCll)
                    .arg(maxFall);
            args << QStringLiteral("-x265-params") << x265p;
        } else {  // HLG
            args << QStringLiteral("-x265-params")
                 << QStringLiteral("repeat-headers=1:colorprim=bt2020:"
                                   "transfer=arib-std-b67:"
                                   "colormatrix=bt2020nc");
        }
    }

    if (haveAudio) {
        QString audioCodec = cfg.value("audioCodec").toString();
        if (audioCodec.isEmpty())
            audioCodec = QStringLiteral("aac");
        args << QStringLiteral("-c:a") << audioCodec;
        const int audioBitrate = cfg.value("audioBitrate").toInt(192);
        args << QStringLiteral("-b:a")
             << QStringLiteral("%1k").arg(audioBitrate);
        // Stop at the shorter of the rendered video / source audio so a
        // longer audio track doesn't pad black frames past the timeline.
        args << QStringLiteral("-shortest");
    }

    args << QStringLiteral("-progress") << QStringLiteral("pipe:2");
    args << job.outputPath;
    return args;
}

QString RenderQueue::findFFmpegBinary()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!path.isEmpty())
        return path;
    const QStringList searchPaths = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/bin")
    };
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"),
                                          searchPaths);
}

// Resolve the Timeline a job renders. Priority:
//   1. job.timeline (the additive in-memory seam) — used as-is, not owned.
//   2. ProjectFile::load(job.projectFilePath) into a fresh heap Timeline via
//      the genuine Timeline::restoreFromProject path (the same call
//      MainWindow::applyLoadedProjectData uses, MainWindow.cpp:3982). The
//      returned Timeline is heap-owned; *ownedOut carries it so the caller
//      deletes it after the render.
Timeline *RenderQueue::resolveTimeline(const RenderJob &job,
                                       Timeline **ownedOut)
{
    if (ownedOut)
        *ownedOut = nullptr;

    if (job.timeline)
        return job.timeline;

    if (job.projectFilePath.isEmpty()
        || !QFile::exists(job.projectFilePath))
        return nullptr;

    ProjectData data;
    if (!ProjectFile::load(job.projectFilePath, data))
        return nullptr;

    auto *tl = new Timeline();
    tl->restoreFromProject(data.videoTracks, data.audioTracks,
                           data.playheadPos, data.markIn, data.markOut,
                           data.zoomLevel);
    if (ownedOut)
        *ownedOut = tl;
    return tl;
}

void RenderQueue::parseFFmpegOutput(const QString &line)
{
    if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
        return;

    // Parse "Duration: HH:MM:SS.ms" to get total duration
    static QRegularExpression durationRx(R"(Duration:\s*(\d+):(\d+):(\d+)\.(\d+))");
    QRegularExpressionMatch durationMatch = durationRx.match(line);
    if (durationMatch.hasMatch()) {
        int hours = durationMatch.captured(1).toInt();
        int minutes = durationMatch.captured(2).toInt();
        int seconds = durationMatch.captured(3).toInt();
        int centis = durationMatch.captured(4).toInt();
        m_currentDuration = hours * 3600.0 + minutes * 60.0 + seconds + centis / 100.0;
        return;
    }

    auto reportPercent = [this](int percent) {
        RenderJob &job = m_jobs[m_currentJobIndex];
        job.progress = percent;
        job.progressPercent = percent;
        emit jobProgress(job.id, percent);
        emit jobProgressUuid(job.uuid, percent);
    };

    // Parse "out_time_ms=XXXX" from -progress pipe output
    static QRegularExpression outTimeMsRx(R"(out_time_ms=(\d+))");
    QRegularExpressionMatch outTimeMatch = outTimeMsRx.match(line);
    if (outTimeMatch.hasMatch() && m_currentDuration > 0.0) {
        double currentUs = outTimeMatch.captured(1).toDouble();
        double currentSec = currentUs / 1000000.0;
        int percent = static_cast<int>(currentSec / m_currentDuration * 100.0);
        percent = qBound(0, percent, 99);
        reportPercent(percent);
        return;
    }

    // Parse "time=HH:MM:SS.ms" from regular stderr output
    static QRegularExpression timeRx(R"(time=\s*(\d+):(\d+):(\d+)\.(\d+))");
    QRegularExpressionMatch timeMatch = timeRx.match(line);
    if (timeMatch.hasMatch() && m_currentDuration > 0.0) {
        int hours = timeMatch.captured(1).toInt();
        int minutes = timeMatch.captured(2).toInt();
        int seconds = timeMatch.captured(3).toInt();
        int centis = timeMatch.captured(4).toInt();
        double currentTime = hours * 3600.0 + minutes * 60.0 + seconds + centis / 100.0;
        int percent = static_cast<int>(currentTime / m_currentDuration * 100.0);
        percent = qBound(0, percent, 99);
        reportPercent(percent);
    }
}

int RenderQueue::findJobIndex(int id) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id == id)
            return i;
    }
    return -1;
}

int RenderQueue::findJobIndexByUuid(const QString &uuid) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].uuid == uuid)
            return i;
    }
    return -1;
}

QString RenderQueue::mapCodecToFFmpeg(const QString &codec)
{
    // The flat-field codec uses Premiere/Resolve naming; map it to the
    // ffmpeg encoder name the existing buildFFmpegArgs() expects.
    if (codec == "h264") return "libx264";
    if (codec == "hevc" || codec == "h265") return "libx265";
    if (codec == "av1") return "libsvtav1";
    if (codec == "prores") return "prores_ks";
    return codec;  // already-prefixed names pass through (libx264, libvpx-vp9...)
}

QString RenderQueue::defaultContainerFor(const QString &codec)
{
    if (codec == "prores") return "mov";
    if (codec == "av1")    return "mp4";
    return "mp4";
}

QVector<RenderPreset> RenderQueue::availablePresets()
{
    // Built-in catalogue — Premiere Media Encoder / Resolve Deliver parity.
    // Bitrates expressed in bps to match the spec.
    return {
        { "1080p H.264 / 50 Mbps",            1920, 1080, "h264",   50000000,  "mp4" },
        { "1080p H.265 / 30 Mbps",            1920, 1080, "hevc",   30000000,  "mp4" },
        { "4K H.264 / 100 Mbps",              3840, 2160, "h264",  100000000,  "mp4" },
        { "4K H.265 / 60 Mbps",               3840, 2160, "hevc",   60000000,  "mp4" },
        { "720p H.264 / 8 Mbps (web)",        1280,  720, "h264",    8000000,  "mp4" },
        { "ProRes 422 LT",                    1920, 1080, "prores", 100000000,  "mov" },
        { "AV1 4K / 30 Mbps",                 3840, 2160, "av1",    30000000,  "mp4" },
        { "Twitter 720p H.264 / 6 Mbps",      1280,  720, "h264",    6000000,  "mp4" },
        { "YouTube 4K HDR (HEVC) / 80 Mbps",  3840, 2160, "hevc",   80000000,  "mp4" },
        { "Vimeo 1080p H.264 / 20 Mbps",      1920, 1080, "h264",   20000000,  "mp4" },
    };
}

RenderJob RenderQueue::jobFromPreset(const RenderPreset &preset,
                                     const QString &outputPath,
                                     qint64 startUs,
                                     qint64 endUs)
{
    RenderJob j;
    j.uuid = makeUuid();
    j.outputPath = outputPath;
    j.preset = preset.name;
    j.width = preset.width;
    j.height = preset.height;
    j.codec = preset.codec;
    j.bitrateBps = preset.bitrateBps;
    j.startUs = startUs;
    j.endUs = endUs;
    j.passes = 1;
    j.status = RenderJobStatus::Pending;
    j.name = preset.name;
    return j;
}
