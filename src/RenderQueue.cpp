#include "RenderQueue.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>

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
    if (m_process) {
        QObject::disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->waitForFinished(3000);
        delete m_process;
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
    m_running = false;
    m_paused = false;
    cancelCurrent();
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
    const bool hadRunningJob = m_running;
    m_running = false;
    m_paused = false;
    if (hadRunningJob)
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
    if (!m_process || m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
        return;

    RenderJob &j = m_jobs[m_currentJobIndex];
    if (j.status == RenderJobStatus::Rendering) {
        j.status = RenderJobStatus::Cancelled;
        j.endTime = QDateTime::currentDateTime();
        j.errorMessage = "Cancelled by user";
        j.error = j.errorMessage;
        emit jobsChanged();
    }

    QProcess *process = m_process;
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(3000);
    }

    // Don't auto-advance; let the process finished handler deal with it
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
    if (!m_running) {
        m_currentJobIndex = -1;
        return;
    }

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

    emit jobStarted(job.id);
    emit jobsChanged();

    // Build ffmpeg command
    QStringList args = buildFFmpegArgs(job);

    if (m_process) {
        delete m_process;
        m_process = nullptr;
    }

    m_process = new QProcess(this);
    QProcess *process = m_process;
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        if (process != m_process)
            return;
        while (process->canReadLine()) {
            QString line = QString::fromUtf8(process->readLine()).trimmed();
            parseFFmpegOutput(line);
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        if (process != m_process)
            return;
        while (process->canReadLine()) {
            QString line = QString::fromUtf8(process->readLine()).trimmed();
            parseFFmpegOutput(line);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        if (process == m_process)
            m_process = nullptr;
        process->deleteLater();

        if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
            return;

        RenderJob &job = m_jobs[m_currentJobIndex];
        job.endTime = QDateTime::currentDateTime();

        if (job.status == RenderJobStatus::Cancelled) {
            // Already marked cancelled by cancelCurrent()
            emit jobCompletedUuid(job.uuid, false, job.error);
        } else if (exitStatus == QProcess::CrashExit || exitCode != 0) {
            job.status = RenderJobStatus::Failed;
            job.errorMessage = QString("FFmpeg exited with code %1").arg(exitCode);
            job.error = job.errorMessage;
            emit jobFailed(job.id, job.errorMessage);
            emit jobCompletedUuid(job.uuid, false, job.errorMessage);
        } else {
            job.status = RenderJobStatus::Completed;
            job.progress = 100;
            job.progressPercent = 100;
            emit jobCompleted(job.id);
            emit jobCompletedUuid(job.uuid, true, QString());
        }

        emit jobsChanged();

        // Emit overall queue progress
        int total = m_jobs.size();
        int done = completedCount();
        for (const auto &j : m_jobs) {
            if (j.status == RenderJobStatus::Failed || j.status == RenderJobStatus::Cancelled)
                done++;
        }
        if (total > 0)
            emit queueProgress(done * 100 / total);

        // Advance to next job only while the queue is still running.
        if (m_running)
            startNextJob();
    });

    process->start("ffmpeg", args);
    if (!process->waitForStarted(5000)) {
        job.status = RenderJobStatus::Failed;
        job.endTime = QDateTime::currentDateTime();
        job.errorMessage = "Failed to start ffmpeg process";
        job.error = job.errorMessage;
        emit jobFailed(job.id, job.errorMessage);
        emit jobCompletedUuid(job.uuid, false, job.errorMessage);
        emit jobsChanged();
        if (process == m_process)
            m_process = nullptr;
        process->deleteLater();
        if (m_running)
            startNextJob();
    }
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

    // RenderQueue launches one ffmpeg process per job, so encoder two-pass mode
    // would lack first-pass stats and fail every time.

    // Progress reporting
    args << "-progress" << "pipe:1";

    // Output
    args << job.outputPath;

    return args;
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
