#include "ProxyManager.h"

namespace {
// Shared between probeDurationUs and probeSourceCodec — once we learn
// ffprobe is missing from PATH, every subsequent probe (regardless of which
// helper) skips its 2 s waitForStarted timeout.
bool g_ffprobeMissing = false;
}

#include <QDateTime>
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
#include <QTextStream>

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
        // Detach the finished/readyRead lambdas first — without this, the
        // old QProcess emits finished() (synchronously when terminate +
        // waitForFinished succeed, asynchronously after kill) and re-enters
        // processNextInQueue() against a m_process pointer the next
        // generateAllProxies has already overwritten with a new QProcess.
        // That ghost lambda is the regenerate crash.
        disconnect(m_process, nullptr, this, nullptr);
        m_process->terminate();
        if (!m_process->waitForFinished(2000))
            m_process->kill();
        m_process->waitForFinished(1000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    // The disconnect above prevents the finished-lambda from running, so any
    // entry still marked Generating will never be reset by that lambda.
    // Reset them explicitly here so generateAllProxies can queue them again.
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it.value().status == ProxyStatus::Generating)
            it.value().status = ProxyStatus::None;
    }

    // Reset synchronously so a follow-up generateAllProxies on the same
    // call stack doesn't see leftover cancel state.
    m_cancelRequested = false;
    m_currentClipName.clear();
    m_currentSourceDurationUs = 0;
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
    const QString srcCodec = probeSourceCodec(originalPath);
    m_cancelRequested = false;
    emit proxyStarted(m_currentClipName);
    if (m_currentSourceDurationUs <= 0) {
        // Probe failed or input has no duration (image, broken file). Push
        // an indeterminate marker so the dialog stops sitting at 0%.
        emit proxyProgress(m_currentClipName, -1);
    }

    // Resolve the encoder we'll actually invoke for this job and log it. The
    // decision mirrors the priority chain in the args composition below.
    const QString jobGpuEnc = chosenGpuH264Encoder();
    QString jobEncoder = jobGpuEnc;
    if (jobEncoder.isEmpty()) {
#if defined(VEDITOR_AV1)
        if (ffmpegHasEncoder("libsvtav1")) jobEncoder = "libsvtav1";
        else
#endif
            jobEncoder = "libx264";
    }

    // Pick the matching hardware decoder for this source codec when the
    // encoder branch is GPU. -hwaccel <api> alone tells ffmpeg "you may use
    // <api>" but doesn't override the decoder selection — AV1 sources still
    // route through libdav1d unless we explicitly say -c:v av1_cuvid. The
    // helpers fall back to QString() when probe fails or the decoder isn't
    // built into ffmpeg, in which case we don't inject anything and let
    // ffmpeg's default decoder run (no regression vs Phase 3).
    //
    // Known cuvid limitations (consumer NVDEC silently rejects these):
    //  - h264_cuvid: H.264 Hi10P (10-bit) and High 4:4:4 Predictive
    //  - hevc_cuvid: HEVC Main 4:4:4 and some HDR10+ streams
    //  - av1_cuvid:  AV1 8/10-bit OK on Ampere+; 12-bit profiles unsupported
    // ffmpeg exits non-zero in those cases; the proxy job is marked Error,
    // partial output is removed, and the queue continues with the next clip.
    auto resolveHwDecoder = [&srcCodec](const char *suffix) -> QString {
        if (srcCodec.isEmpty()) return QString();
        static const QStringList supported = {"av1", "h264", "hevc", "vp9"};
        if (!supported.contains(srcCodec)) return QString();
        const QString candidate = srcCodec + QLatin1Char('_') + QLatin1String(suffix);
        return ffmpegHasDecoder(candidate) ? candidate : QString();
    };
    const QString cuvidDec = (jobGpuEnc == "h264_nvenc") ? resolveHwDecoder("cuvid") : QString();
    const QString qsvDec   = (jobGpuEnc == "h264_qsv")   ? resolveHwDecoder("qsv")   : QString();
    const QString jobDecoder =
        !cuvidDec.isEmpty() ? cuvidDec :
        !qsvDec.isEmpty()   ? qsvDec   :
        srcCodec.isEmpty()  ? QStringLiteral("<probe-failed>") :
                              QStringLiteral("<default>");

    appendEncoderLog(QString("[%1] job source=%2 encoder=%3 decoder=%4")
                     .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                          m_currentClipName,
                          jobEncoder,
                          jobDecoder));

    m_process = new QProcess(this);

    // Encoder priority: GPU H.264 (~5-10x faster than CPU) → AV1 software
    // (smaller, accurate seek; Modern build only) → libx264 software.
    // We probe the actual ffmpeg binary (not the linked libavcodec) because
    // the two can disagree when ffmpeg is on PATH but built independently.
    // Each branch owns its full input chain: GPU encoders need -hwaccel
    // before -i so that decode and scale stay on-card and CPU stays idle.
    QStringList args;
    args << "-y";

    const QString gpuEnc = chosenGpuH264Encoder();
    const QString scaleSize = QString("%1:%2").arg(m_config.proxyWidth).arg(m_config.proxyHeight);

    if (gpuEnc == "h264_nvenc") {
        // Full CUDA pipeline: NVDEC decode → scale_cuda → NVENC encode, no
        // VRAM↔RAM round-trips. Crucial on long sources where software decode
        // and scale would dominate wall-clock even with NVENC encode.
        // The cuvid decoder must be named explicitly: -hwaccel cuda alone
        // doesn't override decoder selection, so AV1/HEVC/VP9 sources would
        // otherwise fall back to libdav1d / libx265 / libvpx.
        args << "-hwaccel" << "cuda"
             << "-hwaccel_output_format" << "cuda";
        if (!cuvidDec.isEmpty())
            args << "-c:v" << cuvidDec;
        args << "-i" << originalPath
             << "-vf" << QString("scale_cuda=%1").arg(scaleSize)
             << "-c:v" << "h264_nvenc"
             << "-preset" << "p1"
             << "-rc" << "constqp" << "-qp" << "28"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    } else if (gpuEnc == "h264_qsv") {
        // Full QSV pipeline: Intel hardware decode → scale_qsv → QSV encode.
        // Per-source qsv decoder is needed for the same reason as cuvid.
        args << "-hwaccel" << "qsv"
             << "-hwaccel_output_format" << "qsv";
        if (!qsvDec.isEmpty())
            args << "-c:v" << qsvDec;
        args << "-i" << originalPath
             << "-vf" << QString("scale_qsv=%1").arg(scaleSize)
             << "-c:v" << "h264_qsv"
             << "-preset" << "veryfast"
             << "-global_quality" << "28"
             << "-c:a" << "aac"
             << "-b:a" << "128k"
             << "-movflags" << "+faststart"
             << entry.proxyPath;
    } else if (gpuEnc == "h264_amf") {
        // AMF doesn't expose a clean GPU scale chain in ffmpeg, so we run
        // d3d11va decode and let the scale fall back to CPU. Still better
        // than full software because decode is the bigger half.
        args << "-hwaccel" << "d3d11va"
             << "-i" << originalPath
             << "-vf" << QString("scale=%1").arg(scaleSize)
             << "-c:v" << "h264_amf"
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
        args << "-i" << originalPath
             << "-vf" << QString("scale=%1").arg(scaleSize)
             << "-c:v" << "libsvtav1"
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
        args << "-i" << originalPath
             << "-vf" << QString("scale=%1").arg(scaleSize)
             << "-c:v" << "libx264"
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

bool ProxyManager::ffmpegHasDecoder(const QString &decoderName)
{
    // Same shape as ffmpegHasEncoder — decoders also can't appear or vanish
    // mid-process, so a one-shot probe per name is sufficient.
    static QHash<QString, bool> cache;
    static bool probeBroken = false;

    auto it = cache.constFind(decoderName);
    if (it != cache.constEnd())
        return it.value();

    if (probeBroken)
        return false;

    QProcess probe;
    probe.start("ffmpeg", QStringList() << "-hide_banner" << "-decoders");
    if (!probe.waitForStarted(2000)) {
        probeBroken = true;
        return false;
    }
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        probeBroken = true;
        return false;
    }

    // `ffmpeg -decoders` lists one entry per line, e.g.
    //   V..... av1_cuvid            Nvidia CUVID AV1 decoder (codec av1)
    // Substring containment on the decoder name is enough: ffmpeg never
    // emits the decoder identifier outside its own column.
    const QByteArray out = probe.readAllStandardOutput()
                         + probe.readAllStandardError();
    const bool has = out.contains(decoderName.toUtf8());
    cache.insert(decoderName, has);
    return has;
}

qint64 ProxyManager::probeDurationUs(const QString &path)
{
    // Cache per source path — same file may be re-queued (settings change,
    // user toggles proxy mode, etc.) and ffprobe spawn is ~200 ms.
    static QHash<QString, qint64> cache;
    // ffprobe-missing flag is shared with probeSourceCodec (file-scope
    // namespace above) so the second helper doesn't re-pay the 2 s spawn
    // timeout on the same machine.
    if (g_ffprobeMissing)
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
        g_ffprobeMissing = true;
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

QString ProxyManager::probeSourceCodec(const QString &path)
{
    // Same caching shape as probeDurationUs — re-queues are common (settings
    // change, proxy mode toggle, etc.) and an ffprobe spawn is ~200 ms.
    static QHash<QString, QString> cache;
    // Share the ffprobe-missing flag with probeDurationUs so we don't
    // re-burn the 2 s waitForStarted on a machine without ffprobe.
    if (g_ffprobeMissing)
        return QString();

    auto it = cache.constFind(path);
    if (it != cache.constEnd())
        return it.value();

    QProcess probe;
    QStringList args;
    args << "-v" << "error"
         << "-select_streams" << "v:0"
         << "-show_entries" << "stream=codec_name"
         << "-of" << "default=noprint_wrappers=1:nokey=1"
         << path;
    probe.start("ffprobe", args);
    if (!probe.waitForStarted(2000)) {
        g_ffprobeMissing = true;
        cache.insert(path, QString());
        return QString();
    }
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        cache.insert(path, QString());
        return QString();
    }
    if (probe.exitCode() != 0) {
        cache.insert(path, QString());
        return QString();
    }

    const QString codec = QString::fromUtf8(
        probe.readAllStandardOutput().trimmed()).toLower();
    cache.insert(path, codec);
    return codec;
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

    appendEncoderLog(QString("[%1] chosen=%2")
                     .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                          cached.isEmpty() ? QStringLiteral("<software-fallback>") : cached));
    return cached;
}

void ProxyManager::appendEncoderLog(const QString &line)
{
    QFile f(proxyDir() + "/encoder_log.txt");
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << line << '\n';
}
