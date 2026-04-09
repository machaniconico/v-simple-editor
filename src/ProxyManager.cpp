#include "ProxyManager.h"
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

    m_process = new QProcess(this);

    QStringList args;
    args << "-y"
         << "-i" << originalPath
         << "-vf" << QString("scale=%1:%2").arg(m_config.proxyWidth).arg(m_config.proxyHeight)
         << "-c:v" << "libx264"
         << "-crf" << QString::number(m_config.quality)
         << "-c:a" << "aac"
         << "-b:a" << "128k"
         << entry.proxyPath;

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, originalPath](int exitCode, QProcess::ExitStatus exitStatus) {

        bool success = (exitCode == 0 && exitStatus == QProcess::NormalExit);

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

        m_process->deleteLater();
        m_process = nullptr;

        processNextInQueue();
    });

    m_process->start("ffmpeg", args);
}
