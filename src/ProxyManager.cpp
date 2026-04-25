#include "ProxyManager.h"
#include "CodecDetector.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QDirIterator>

ProxyManager &ProxyManager::instance()
{
    static ProxyManager mgr;
    return mgr;
}

ProxyManager::ProxyManager()
{
    QDir().mkpath(proxyDir());
    loadIndex();
}

ProxyManager::~ProxyManager()
{
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(3000);
        delete m_process;
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
        delete m_thread;
    }
}

void ProxyManager::setConfig(const ProxyConfig &config)
{
    m_config = config;
}

void ProxyManager::generateProxy(const QString &originalFilePath)
{
    if (!QFile::exists(originalFilePath))
        return;

    if (m_entries.contains(originalFilePath)) {
        auto status = m_entries[originalFilePath].status;
        if (status == ProxyStatus::Ready || status == ProxyStatus::Generating)
            return;
    }

    ProxyEntry entry;
    entry.originalPath = originalFilePath;
    entry.proxyPath = proxyFilePath(originalFilePath);
    entry.proxySize = QSize(m_config.proxyWidth, m_config.proxyHeight);
    entry.status = ProxyStatus::Generating;
    m_entries[originalFilePath] = entry;

    m_queue.append(originalFilePath);
    m_totalQueued = m_queue.size();
    m_completed = 0;

    if (!m_process)
        processNextInQueue();
}

void ProxyManager::generateAllProxies(const QStringList &filePaths)
{
    m_totalQueued = 0;
    m_completed = 0;

    for (const auto &path : filePaths) {
        if (!QFile::exists(path))
            continue;

        if (m_entries.contains(path)) {
            auto status = m_entries[path].status;
            if (status == ProxyStatus::Ready || status == ProxyStatus::Generating)
                continue;
        }

        ProxyEntry entry;
        entry.originalPath = path;
        entry.proxyPath = proxyFilePath(path);
        entry.proxySize = QSize(m_config.proxyWidth, m_config.proxyHeight);
        entry.status = ProxyStatus::Generating;
        m_entries[path] = entry;

        m_queue.append(path);
    }

    m_totalQueued = m_queue.size();
    if (m_totalQueued > 0 && !m_process)
        processNextInQueue();
}

QString ProxyManager::getProxyPath(const QString &originalPath) const
{
    if (m_proxyMode && m_entries.contains(originalPath)) {
        const auto &entry = m_entries[originalPath];
        if (entry.status == ProxyStatus::Ready && QFile::exists(entry.proxyPath))
            return entry.proxyPath;
    }
    return originalPath;
}

bool ProxyManager::hasProxy(const QString &originalPath) const
{
    if (!m_entries.contains(originalPath))
        return false;
    const auto &entry = m_entries[originalPath];
    return entry.status == ProxyStatus::Ready && QFile::exists(entry.proxyPath);
}

void ProxyManager::setProxyMode(bool enabled)
{
    m_proxyMode = enabled;
}

void ProxyManager::deleteProxy(const QString &originalPath)
{
    if (!m_entries.contains(originalPath))
        return;

    const auto &entry = m_entries[originalPath];
    QFile::remove(entry.proxyPath);
    m_entries.remove(originalPath);
    saveIndex();
}

void ProxyManager::deleteAllProxies()
{
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        QFile::remove(it.value().proxyPath);

    m_entries.clear();
    saveIndex();
}

QString ProxyManager::proxyDir()
{
    return QDir::homePath() + "/.veditor/proxies";
}

qint64 ProxyManager::diskUsage() const
{
    qint64 total = 0;
    QDirIterator it(proxyDir(), QDir::Files);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// --- private ---

QString ProxyManager::hashPath(const QString &path) const
{
    QByteArray data = path.toUtf8();
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString ProxyManager::proxyFilePath(const QString &originalPath) const
{
    QString hash = hashPath(originalPath);
    return proxyDir() + "/" + hash + "." + m_config.format;
}

void ProxyManager::loadIndex()
{
    QString indexPath = proxyDir() + "/index.json";
    QFile file(indexPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return;

    QJsonObject root = doc.object();
    QJsonArray entries = root["entries"].toArray();

    for (const auto &val : entries) {
        QJsonObject obj = val.toObject();
        ProxyEntry entry;
        entry.originalPath = obj["originalPath"].toString();
        entry.proxyPath = obj["proxyPath"].toString();
        entry.originalSize = QSize(obj["originalWidth"].toInt(), obj["originalHeight"].toInt());
        entry.proxySize = QSize(obj["proxyWidth"].toInt(), obj["proxyHeight"].toInt());

        // Verify the proxy file still exists on disk
        if (QFile::exists(entry.proxyPath))
            entry.status = ProxyStatus::Ready;
        else
            entry.status = ProxyStatus::None;

        m_entries[entry.originalPath] = entry;
    }
}

void ProxyManager::saveIndex()
{
    QString indexPath = proxyDir() + "/index.json";
    QJsonArray entries;

    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const auto &entry = it.value();
        if (entry.status != ProxyStatus::Ready)
            continue;

        QJsonObject obj;
        obj["originalPath"] = entry.originalPath;
        obj["proxyPath"] = entry.proxyPath;
        obj["originalWidth"] = entry.originalSize.width();
        obj["originalHeight"] = entry.originalSize.height();
        obj["proxyWidth"] = entry.proxySize.width();
        obj["proxyHeight"] = entry.proxySize.height();
        entries.append(obj);
    }

    QJsonObject root;
    root["entries"] = entries;

    QFile file(indexPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
        file.close();
    }
}

void ProxyManager::cancelGeneration()
{
    m_cancelRequested = true;
    m_queue.clear();
    if (m_process) {
        m_process->terminate();
        if (!m_process->waitForFinished(2000))
            m_process->kill();
    }
}

void ProxyManager::parseFfmpegProgress(const QByteArray &chunk)
{
    // ffmpeg -progress pipe:1 emits key=value lines. We care about
    // out_time_ms (presentation time in microseconds).
    const QList<QByteArray> lines = chunk.split('\n');
    for (const QByteArray &line : lines) {
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        const QByteArray key = line.left(eq).trimmed();
        const QByteArray val = line.mid(eq + 1).trimmed();
        if (key != "out_time_ms" && key != "out_time_us") continue;
        bool ok = false;
        const qint64 us = val.toLongLong(&ok);
        if (!ok || m_currentSourceDurationUs <= 0) continue;
        qint64 pct = us * 100 / m_currentSourceDurationUs;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        emit proxyProgress(m_currentClipName, static_cast<int>(pct));
    }
}

void ProxyManager::processNextInQueue()
{
    if (m_queue.isEmpty()) {
        emit allProxiesReady();
        return;
    }

    QString originalPath = m_queue.takeFirst();
    if (!m_entries.contains(originalPath)) {
        processNextInQueue();
        return;
    }

    auto &entry = m_entries[originalPath];
    entry.status = ProxyStatus::Generating;

    m_currentClipName = QFileInfo(originalPath).fileName();
    m_currentSourceDurationUs = 0; // will be filled by ffprobe-lite below if available
    m_cancelRequested = false;
    emit proxyStarted(m_currentClipName);

    m_process = new QProcess(this);

    QStringList args;
    args << "-y"
         << "-i" << originalPath
         << "-vf" << QString("scale=%1:%2").arg(m_config.proxyWidth).arg(m_config.proxyHeight);

#if defined(VEDITOR_AV1)
    // Modern Edition: AV1 proxy via SVT-AV1 with sidx fragmented MP4 for accurate seeking.
    // Runtime fallback to H.264 if the linked ffmpeg lacks libsvtav1.
    const bool av1Available = CodecDetector::isEncoderAvailable("libsvtav1");
    if (av1Available) {
        args << "-c:v" << "libsvtav1"
             << "-preset" << "8"
             << "-crf" << "35"
             << "-g" << "120"
             << "-pix_fmt" << "yuv420p"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart+sidx+frag_keyframe+empty_moov+default_base_moof"
             << entry.proxyPath;
    } else
#endif
    {
        // Classic / fallback: H.264 proxy. -g 120 forces keyframes every ~4s and
        // +faststart ensures accurate seek (past bug: missing keyframes broke seekbar).
        args << "-c:v" << "libx264"
             << "-crf" << QString::number(m_config.quality)
             << "-g" << "120"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    }

    // ffmpeg's -progress pipe:1 emits key=value progress lines on stdout
    // (out_time_us, frame, fps, ...) which feed proxyProgress for the UI.
    args.prepend("pipe:1");
    args.prepend("-progress");

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_process) return;
        parseFfmpegProgress(m_process->readAllStandardOutput());
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, originalPath](int exitCode, QProcess::ExitStatus exitStatus) {

        const bool wasCancelled = m_cancelRequested;
        const bool success = !wasCancelled
            && (exitCode == 0 && exitStatus == QProcess::NormalExit);
        const QString clipName = m_currentClipName;

        if (m_entries.contains(originalPath)) {
            auto &entry = m_entries[originalPath];
            if (success && QFile::exists(entry.proxyPath)) {
                entry.status = ProxyStatus::Ready;
                saveIndex();
                emit proxyGenerated(originalPath, entry.proxyPath);
            } else {
                entry.status = ProxyStatus::Error;
                QFile::remove(entry.proxyPath);
            }
        }

        m_completed++;
        if (m_totalQueued > 0) {
            int percent = static_cast<int>(m_completed * 100 / m_totalQueued);
            emit progressChanged(percent);
        }

        if (wasCancelled)
            emit proxyCancelled(clipName);
        else
            emit proxyFinished(clipName, success);

        m_process->deleteLater();
        m_process = nullptr;
        m_currentClipName.clear();
        m_currentSourceDurationUs = 0;

        if (wasCancelled) {
            m_cancelRequested = false;
            return; // queue was cleared in cancelGeneration().
        }

        processNextInQueue();
    });

    m_process->start("ffmpeg", args);
}
