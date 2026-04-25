#include "ProxyManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
    m_currentSourceDurationUs = probeDurationUs(originalPath);
    m_cancelRequested = false;
    emit proxyStarted(m_currentClipName);
    if (m_currentSourceDurationUs <= 0) {
        // Probe failed or input has no duration (image, broken file). Push
        // an indeterminate marker so the dialog stops sitting at 0%.
        emit proxyProgress(m_currentClipName, -1);
    }

    m_process = new QProcess(this);

    QStringList args;
    args << "-y"
         << "-i" << originalPath
         << "-vf" << QString("scale=%1:%2").arg(m_config.proxyWidth).arg(m_config.proxyHeight);

    // Encoder priority: GPU H.264 (~5-10x faster than CPU) → AV1 software
    // (smaller, accurate seek; Modern build only) → libx264 software.
    // We probe the actual ffmpeg binary (not the linked libavcodec) because
    // the two can disagree when ffmpeg is on PATH but built independently.
    const QString gpuEnc = chosenGpuH264Encoder();
    if (gpuEnc == "h264_nvenc") {
        args << "-c:v" << "h264_nvenc"
             << "-preset" << "p1"
             << "-tune" << "hq"
             << "-rc" << "constqp" << "-qp" << "28"
             << "-pix_fmt" << "yuv420p"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    } else if (gpuEnc == "h264_qsv") {
        args << "-c:v" << "h264_qsv"
             << "-preset" << "veryfast"
             << "-global_quality" << "28"
             << "-pix_fmt" << "nv12"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    } else if (gpuEnc == "h264_amf") {
        args << "-c:v" << "h264_amf"
             << "-quality" << "speed"
             << "-rc" << "cqp" << "-qp_i" << "28" << "-qp_p" << "28"
             << "-pix_fmt" << "yuv420p"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    }
#if defined(VEDITOR_AV1)
    else if (ffmpegHasEncoder("libsvtav1")) {
        // preset 12: near-fastest SVT-AV1 — proxies trade quality for speed.
        args << "-c:v" << "libsvtav1"
             << "-preset" << "12"
             << "-crf" << "35"
             << "-g" << "120"
             << "-pix_fmt" << "yuv420p"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart+frag_keyframe+empty_moov+default_base_moof"
             << entry.proxyPath;
    }
#endif
    else {
        // Last-resort software H.264 — slowest but always available. -g 120
        // forces keyframes every ~4 s so the seekbar stays accurate.
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

bool ProxyManager::ffmpegHasEncoder(const QString &encoderName)
{
    // Cache per encoder name — ffmpeg's encoder list doesn't change at
    // runtime so a one-shot probe is enough.
    static QHash<QString, bool> cache;
    static bool probeBroken = false;

    auto it = cache.constFind(encoderName);
    if (it != cache.constEnd())
        return it.value();

    // If a previous probe couldn't even spawn ffmpeg, don't keep retrying —
    // the user has bigger problems and we should fall back to libx264.
    if (probeBroken)
        return false;

    QProcess probe;
    probe.start("ffmpeg", QStringList() << "-hide_banner" << "-encoders");
    if (!probe.waitForStarted(2000)) {
        probeBroken = true;
        return false;
    }
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        probeBroken = true;
        return false;
    }

    // `ffmpeg -encoders` output lists one encoder per line, e.g.
    //   V..... libsvtav1            SVT-AV1 encoder (codec av1)
    // A simple substring search on the encoder name is sufficient because
    // ffmpeg never emits the encoder identifier outside its own column.
    const QByteArray out = probe.readAllStandardOutput()
                         + probe.readAllStandardError();
    const bool has = out.contains(encoderName.toUtf8());
    cache.insert(encoderName, has);
    return has;
}

qint64 ProxyManager::probeDurationUs(const QString &path)
{
    // Cache per source path — same file may be re-queued (settings change,
    // user toggles proxy mode, etc.) and ffprobe spawn is ~200 ms.
    static QHash<QString, qint64> cache;
    // Mirrors the probeBroken pattern in ffmpegHasEncoder: once we learn
    // ffprobe is not on PATH, stop burning a 2 s waitForStarted per clip.
    static bool ffprobeMissing = false;
    if (ffprobeMissing)
        return 0;

    auto it = cache.constFind(path);
    if (it != cache.constEnd())
        return it.value();

    QProcess probe;
    QStringList args;
    args << "-v" << "error"
         << "-show_entries" << "format=duration"
         << "-of" << "default=noprint_wrappers=1:nokey=1"
         << path;
    probe.start("ffprobe", args);
    if (!probe.waitForStarted(2000)) {
        ffprobeMissing = true;
        cache.insert(path, 0);
        return 0;
    }
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        cache.insert(path, 0);
        return 0;
    }
    if (probe.exitCode() != 0) {
        cache.insert(path, 0);
        return 0;
    }

    const QByteArray out = probe.readAllStandardOutput().trimmed();
    bool ok = false;
    const double seconds = out.toDouble(&ok);
    if (!ok || seconds <= 0.0) {
        cache.insert(path, 0);
        return 0;
    }

    const qint64 us = static_cast<qint64>(seconds * 1'000'000.0);
    cache.insert(path, us);
    return us;
}

QString ProxyManager::chosenGpuH264Encoder()
{
    static QString cached;
    static bool probed = false;
    if (probed)
        return cached;
    probed = true;

    if (ffmpegHasEncoder("h264_nvenc")) cached = "h264_nvenc";
    else if (ffmpegHasEncoder("h264_qsv")) cached = "h264_qsv";
    else if (ffmpegHasEncoder("h264_amf")) cached = "h264_amf";
    return cached;
}
