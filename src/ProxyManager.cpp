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
    detectHwEncoder();
    qInfo() << "[PROXY] proxyDir =" << proxyDir()
            << "existing entries =" << m_entries.size()
            << "hwEncoder =" << (m_hwEncoder.isEmpty() ? QString("(SW libx264)") : m_hwEncoder);
}

void ProxyManager::detectHwEncoder()
{
    // Stage 2.8.1 — run `ffmpeg -hide_banner -encoders` once and grep for
    // the known HW H.264 encoders. ffmpeg prints the encoder name in the
    // column after the capability flags. NVENC wins over QSV over AMF
    // since it's the most common and usually fastest.
    QProcess probe;
    probe.start("ffmpeg", {"-hide_banner", "-encoders"});
    if (!probe.waitForFinished(5000)) {
        qWarning() << "[PROXY] ffmpeg -encoders probe timed out; using SW libx264";
        return;
    }
    const QString out = QString::fromLocal8Bit(probe.readAllStandardOutput());
    const char *candidates[] = { "h264_nvenc", "h264_qsv", "h264_amf" };
    for (const char *enc : candidates) {
        if (out.contains(enc)) {
            m_hwEncoder = QString::fromLatin1(enc);
            return;
        }
    }
}

ProxyManager::~ProxyManager()
{
    // Stage 2.8.2 — DON'T kill ffmpeg on app close. On Windows the child
    // process survives parent exit, and a 113-min source takes ~6 min at
    // NVENC 20x realtime — far longer than the user will keep the app open
    // during editing. Instead we disconnect our signal handlers, leak the
    // QProcess (OS reclaims on app exit), and persist the Generating state
    // so the next launch can verify and reuse the finalized proxy.
    if (m_process) {
        m_process->disconnect();
        m_process->setParent(nullptr);
        // Intentional leak: Qt would kill the child on QProcess dtor.
        m_process = nullptr;
    }
    saveIndex();
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
    if (!QFile::exists(originalFilePath)) {
        qWarning() << "[PROXY] skip (file missing):" << originalFilePath;
        return;
    }

    if (m_entries.contains(originalFilePath)) {
        auto status = m_entries[originalFilePath].status;
        if (status == ProxyStatus::Ready) {
            qInfo() << "[PROXY] skip (already Ready):" << originalFilePath;
            return;
        }
        if (status == ProxyStatus::Generating) {
            qInfo() << "[PROXY] skip (already Generating):" << originalFilePath;
            return;
        }
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

    qInfo() << "[PROXY] queued:" << originalFilePath
            << "-> proxy:" << entry.proxyPath;

    // Stage 2.8.2 — persist Generating state so a subsequent launch can
    // verify-and-resume any ffmpeg that kept running past app close.
    saveIndex();

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

        const QString savedStatus = obj["status"].toString("Ready");
        if (!QFile::exists(entry.proxyPath)) {
            // File disappeared between runs — force re-queue.
            entry.status = ProxyStatus::None;
        } else if (savedStatus == "Generating") {
            // Stage 2.8.2 — ffmpeg may have finished after app close on
            // Windows. Probe with ffprobe: if the container is valid the
            // proxy is usable, otherwise discard and re-queue.
            QProcess probe;
            probe.start("ffprobe", {"-v", "error",
                                    "-show_entries", "format=duration",
                                    "-of", "default=nw=1:nk=1",
                                    entry.proxyPath});
            if (probe.waitForFinished(5000) && probe.exitCode() == 0) {
                entry.status = ProxyStatus::Ready;
                qInfo() << "[PROXY] resumed Ready from previous session:"
                        << entry.proxyPath;
            } else {
                qInfo() << "[PROXY] discarding incomplete proxy from previous session:"
                        << entry.proxyPath;
                QFile::remove(entry.proxyPath);
                entry.status = ProxyStatus::None;
            }
        } else {
            entry.status = ProxyStatus::Ready;
        }

        m_entries[entry.originalPath] = entry;
    }
}

void ProxyManager::saveIndex()
{
    QString indexPath = proxyDir() + "/index.json";
    QJsonArray entries;

    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const auto &entry = it.value();
        // Stage 2.8.2 — also persist Generating so the next launch can
        // verify the partial file on disk (ffmpeg may have finished after
        // app close on Windows). Error entries are ignored here so they
        // re-queue fresh next time.
        if (entry.status != ProxyStatus::Ready
            && entry.status != ProxyStatus::Generating)
            continue;

        QJsonObject obj;
        obj["originalPath"] = entry.originalPath;
        obj["proxyPath"] = entry.proxyPath;
        obj["originalWidth"] = entry.originalSize.width();
        obj["originalHeight"] = entry.originalSize.height();
        obj["proxyWidth"] = entry.proxySize.width();
        obj["proxyHeight"] = entry.proxySize.height();
        obj["status"] = (entry.status == ProxyStatus::Ready) ? "Ready" : "Generating";
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

    // Stage 2.8 — rewrite the proxy container extension to match the chosen
    // codec so H.264/ProRes/DNxHR don't collide and QFile::exists checks on
    // the saved index stay honest.
    const QString hashStem = QFileInfo(entry.proxyPath).completeBaseName();
    QString ext;
    QStringList codecArgs;
    switch (m_config.codec) {
    case ProxyCodec::H264:
        ext = "mp4";
        if (!m_hwEncoder.isEmpty()) {
            // Stage 2.8.1 — GPU encode is 50-100x realtime vs libx264
            // veryfast at 8x realtime. Each encoder has its own CQ/QP knob.
            codecArgs << "-c:v" << m_hwEncoder;
            if (m_hwEncoder == "h264_nvenc") {
                codecArgs << "-preset" << "p2"
                          << "-tune" << "ll"
                          << "-rc" << "vbr"
                          << "-cq" << QString::number(m_config.quality);
            } else if (m_hwEncoder == "h264_qsv") {
                codecArgs << "-preset" << "veryfast"
                          << "-global_quality" << QString::number(m_config.quality);
            } else if (m_hwEncoder == "h264_amf") {
                codecArgs << "-quality" << "speed"
                          << "-rc" << "cqp"
                          << "-qp_i" << QString::number(m_config.quality)
                          << "-qp_p" << QString::number(m_config.quality);
            }
            codecArgs << "-c:a" << "aac" << "-b:a" << "128k";
        } else {
            // SW fallback — ultrafast is ~1.5x faster than veryfast for a
            // modest file-size cost, and proxies are short-lived anyway.
            codecArgs << "-c:v" << "libx264"
                      << "-crf" << QString::number(m_config.quality)
                      << "-preset" << "ultrafast"
                      << "-threads" << "0"         // use all cores
                      << "-c:a" << "aac" << "-b:a" << "128k";
        }
        // Stage 2.8.3 — fragmented MP4 so interrupted encodes still produce
        // a playable (if truncated) proxy. Every keyframe flushes a new
        // fragment with its own moof header, so even if the user closes
        // the app 30 s into a 6 min transcode we still have 30 s of usable
        // proxy next launch (ffprobe accepts it → Stage 2.8.2 resume logic
        // promotes it to Ready).
        codecArgs << "-movflags" << "+frag_keyframe+empty_moov+default_base_moof";
        break;
    case ProxyCodec::ProResLT:
        ext = "mov";
        codecArgs << "-c:v" << "prores_ks"
                  << "-profile:v" << "1"            // LT
                  << "-pix_fmt" << "yuv422p10le"
                  << "-qscale:v" << "12"
                  << "-c:a" << "pcm_s16le";
        break;
    case ProxyCodec::DNxHRLB:
        ext = "mov";
        codecArgs << "-c:v" << "dnxhd"
                  << "-profile:v" << "dnxhr_lb"
                  << "-pix_fmt" << "yuv422p"
                  << "-c:a" << "pcm_s16le";
        break;
    }
    entry.proxyPath = proxyDir() + "/" + hashStem + "." + ext;

    m_process = new QProcess(this);

    QStringList args;
    args << "-y"
         << "-i" << originalPath
         << "-vf" << QString("scale=%1:-2").arg(m_config.proxyWidth);
    args += codecArgs;
    args << entry.proxyPath;

    qInfo() << "[PROXY] ffmpeg start:" << args.join(" ");

    connect(m_process, &QProcess::errorOccurred,
            this, [this, originalPath](QProcess::ProcessError err) {
        qWarning() << "[PROXY] errorOccurred for" << originalPath
                   << "err=" << static_cast<int>(err)
                   << (m_process ? m_process->errorString() : QString("(no process)"));
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, originalPath](int exitCode, QProcess::ExitStatus exitStatus) {

        bool success = (exitCode == 0 && exitStatus == QProcess::NormalExit);

        if (m_entries.contains(originalPath)) {
            auto &entry = m_entries[originalPath];
            if (success && QFile::exists(entry.proxyPath)) {
                entry.status = ProxyStatus::Ready;
                saveIndex();
                qInfo() << "[PROXY] ready:" << originalPath
                        << "->" << entry.proxyPath
                        << QString("(%1 bytes)").arg(QFileInfo(entry.proxyPath).size());
                emit proxyGenerated(originalPath, entry.proxyPath);
            } else {
                entry.status = ProxyStatus::Error;
                const QByteArray stderrTail =
                    m_process ? m_process->readAllStandardError().right(600) : QByteArray();
                qWarning() << "[PROXY] FAILED for" << originalPath
                           << "exit=" << exitCode
                           << "status=" << exitStatus
                           << "stderr(tail):" << stderrTail;
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
