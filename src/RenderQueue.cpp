#include "RenderQueue.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>

RenderQueue::RenderQueue(QObject *parent)
    : QObject(parent)
{
}

RenderQueue::~RenderQueue()
{
    if (m_process) {
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
    job.name = name;
    job.projectFilePath = projectFilePath;
    job.outputPath = outputPath;
    job.exportConfig = exportConfig;
    job.status = RenderJobStatus::Pending;
    job.progress = 0;

    m_jobs.append(job);
    return job.id;
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
}

void RenderQueue::clearAll()
{
    if (m_running)
        cancelCurrent();

    m_jobs.clear();
    m_currentJobIndex = -1;
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
    if (!m_process || m_currentJobIndex < 0)
        return;

    m_process->kill();
    m_process->waitForFinished(3000);

    m_jobs[m_currentJobIndex].status = RenderJobStatus::Cancelled;
    m_jobs[m_currentJobIndex].endTime = QDateTime::currentDateTime();
    m_jobs[m_currentJobIndex].errorMessage = "Cancelled by user";

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
}

void RenderQueue::moveJobDown(int id)
{
    int idx = findJobIndex(id);
    if (idx < 0 || idx >= m_jobs.size() - 1)
        return;

    if (m_jobs[idx].status != RenderJobStatus::Pending)
        return;

    m_jobs.swapItemsAt(idx, idx + 1);
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
        obj["id"] = job.id;
        obj["name"] = job.name;
        obj["projectFilePath"] = job.projectFilePath;
        obj["outputPath"] = job.outputPath;
        obj["exportConfig"] = job.exportConfig;
        obj["status"] = static_cast<int>(job.status);
        obj["progress"] = job.progress;
        obj["errorMessage"] = job.errorMessage;

        if (job.startTime.isValid())
            obj["startTime"] = job.startTime.toString(Qt::ISODate);
        if (job.endTime.isValid())
            obj["endTime"] = job.endTime.toString(Qt::ISODate);

        jobsArray.append(obj);
    }

    QJsonObject root;
    root["version"] = 1;
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
        job.name = obj["name"].toString();
        job.projectFilePath = obj["projectFilePath"].toString();
        job.outputPath = obj["outputPath"].toString();
        job.exportConfig = obj["exportConfig"].toObject();
        job.status = static_cast<RenderJobStatus>(obj["status"].toInt());
        job.progress = obj["progress"].toInt();
        job.errorMessage = obj["errorMessage"].toString();

        if (obj.contains("startTime"))
            job.startTime = QDateTime::fromString(obj["startTime"].toString(), Qt::ISODate);
        if (obj.contains("endTime"))
            job.endTime = QDateTime::fromString(obj["endTime"].toString(), Qt::ISODate);

        m_jobs.append(job);
    }

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
        return;
    }

    RenderJob &job = m_jobs[m_currentJobIndex];
    job.status = RenderJobStatus::Rendering;
    job.progress = 0;
    job.startTime = QDateTime::currentDateTime();
    m_currentDuration = 0.0;

    emit jobStarted(job.id);

    // Build ffmpeg command
    QStringList args = buildFFmpegArgs(job);

    if (m_process) {
        delete m_process;
        m_process = nullptr;
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        while (m_process->canReadLine()) {
            QString line = QString::fromUtf8(m_process->readLine()).trimmed();
            parseFFmpegOutput(line);
        }
    });

    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        while (m_process->canReadLine()) {
            QString line = QString::fromUtf8(m_process->readLine()).trimmed();
            parseFFmpegOutput(line);
        }
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_currentJobIndex < 0 || m_currentJobIndex >= m_jobs.size())
            return;

        RenderJob &job = m_jobs[m_currentJobIndex];
        job.endTime = QDateTime::currentDateTime();

        if (job.status == RenderJobStatus::Cancelled) {
            // Already marked cancelled by cancelCurrent()
        } else if (exitStatus == QProcess::CrashExit || exitCode != 0) {
            job.status = RenderJobStatus::Failed;
            job.errorMessage = QString("FFmpeg exited with code %1").arg(exitCode);
            emit jobFailed(job.id, job.errorMessage);
        } else {
            job.status = RenderJobStatus::Completed;
            job.progress = 100;
            emit jobCompleted(job.id);
        }

        // Emit overall queue progress
        int total = m_jobs.size();
        int done = completedCount();
        for (const auto &j : m_jobs) {
            if (j.status == RenderJobStatus::Failed || j.status == RenderJobStatus::Cancelled)
                done++;
        }
        if (total > 0)
            emit queueProgress(done * 100 / total);

        // Advance to next job
        startNextJob();
    });

    m_process->start("ffmpeg", args);
    if (!m_process->waitForStarted(5000)) {
        job.status = RenderJobStatus::Failed;
        job.endTime = QDateTime::currentDateTime();
        job.errorMessage = "Failed to start ffmpeg process";
        emit jobFailed(job.id, job.errorMessage);
        startNextJob();
    }
}

QStringList RenderQueue::buildFFmpegArgs(const RenderJob &job) const
{
    QStringList args;
    args << "-y";  // overwrite output

    const QJsonObject &cfg = job.exportConfig;

    // Input file — use project file path as source
    args << "-i" << job.projectFilePath;

    // Video codec
    QString videoCodec = cfg["videoCodec"].toString("libx264");
    args << "-c:v" << videoCodec;

    // Video bitrate
    int videoBitrate = cfg["videoBitrate"].toInt(10000);
    args << "-b:v" << QString("%1k").arg(videoBitrate);

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

    // Parse "out_time_ms=XXXX" from -progress pipe output
    static QRegularExpression outTimeMsRx(R"(out_time_ms=(\d+))");
    QRegularExpressionMatch outTimeMatch = outTimeMsRx.match(line);
    if (outTimeMatch.hasMatch() && m_currentDuration > 0.0) {
        double currentUs = outTimeMatch.captured(1).toDouble();
        double currentSec = currentUs / 1000000.0;
        int percent = static_cast<int>(currentSec / m_currentDuration * 100.0);
        percent = qBound(0, percent, 99);

        RenderJob &job = m_jobs[m_currentJobIndex];
        job.progress = percent;
        emit jobProgress(job.id, percent);
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

        RenderJob &job = m_jobs[m_currentJobIndex];
        job.progress = percent;
        emit jobProgress(job.id, percent);
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
