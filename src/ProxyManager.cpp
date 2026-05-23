#include "ProxyManager.h"

#include "libavcore/Decode.h"
#include "libavcore/Encode.h"
#include "libavcore/Probe.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

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
#include <QSettings>
#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
}

namespace {
// proxyDir() used to QSettings + QFileInfo + isDir() on every call, which
// landed in hot paths (proxyFilePath → getProxyPath at preview time). The
// revision counter is bumped by setProxyStorageDir(); proxyDir() can then
// short-circuit to the cached value while the revision is unchanged.
// std::atomic so a future worker thread that touches paths can't see a
// torn revision read.
std::atomic<int> g_proxyDirRevision{0};
QString g_cachedProxyDir;
int g_cachedProxyDirRevision = -1;
// Guards both g_cachedProxyDir and g_cachedProxyDirRevision. The QString
// itself is implicitly-shared so a concurrent read while another thread
// reassigns it would touch its refcount unsafely (Gemini review,
// 2026-04-25). proxyDir() is static so we can't rely on instance-level
// scheduling — defend at the data instead.
QMutex g_proxyDirMutex;

struct ProxyTranscodeJob {
    QString originalPath;
    QString proxyPath;
    QString clipName;
    ProxyConfig config;
    QualityPreset preset = QualityPreset::Medium;
    qint64 durationUs = 0;
};

struct ProxyTranscodeResult {
    bool success = false;
    bool cancelled = false;
    QString error;
    QString activeEncoderName;
    int framesEncoded = 0;
};

bool proxyEncoderAvailableForInProcess(const std::string &name)
{
    // The bundled libavcodec build exposes only Media Foundation H.264 on
    // Windows and software libx264 elsewhere; NVENC/QSV/AMF GPU encoders are
    // not in the registry. Reject them up-front so a stale user override does
    // not propagate into an unavailable encoder request to FrameEncoder.
    if (name == "h264_nvenc" || name == "h264_qsv" || name == "h264_amf")
        return false;
    // Prefer the in-process Media Foundation path when present; libx264 is
    // only a non-Windows/developer fallback for builds without h264_mf.
    if (name == "libx264" && libavcore::encoderAvailable("h264_mf"))
        return false;
    return libavcore::encoderAvailable(name);
}

QString inProcessH264FallbackEncoderName()
{
    static const char *const kOrder[] = {
        "h264_mf",
        "libx264",
        "mpeg4",
    };
    for (const char *name : kOrder) {
        if (proxyEncoderAvailableForInProcess(name))
            return QString::fromLatin1(name);
    }
    return QStringLiteral("libx264");
}

int fpsFromRational(AVRational rate)
{
    if (rate.num <= 0 || rate.den <= 0)
        return 30;
    const double fps = av_q2d(rate);
    if (!std::isfinite(fps) || fps <= 0.0)
        return 30;
    return std::clamp(static_cast<int>(std::lround(fps)), 1, 240);
}

int64_t proxyVideoBitrateBits(const ProxyConfig &config,
                              QualityPreset preset,
                              int fps)
{
    const double bitsPerPixel =
        (preset == QualityPreset::High) ? 0.18 :
        (preset == QualityPreset::Low)  ? 0.07 : 0.11;
    const int safeFps = std::clamp(fps, 1, 240);
    const double target =
        static_cast<double>(std::max(1, config.proxyWidth))
        * static_cast<double>(std::max(1, config.proxyHeight))
        * static_cast<double>(safeFps)
        * bitsPerPixel;

    const int64_t floor =
        (preset == QualityPreset::High) ? 1'200'000 :
        (preset == QualityPreset::Low)  ?   400'000 : 800'000;
    return std::max<int64_t>(floor, static_cast<int64_t>(target));
}

void postProxyProgress(ProxyManager *manager,
                       const QString &clipName,
                       int percent)
{
    QMetaObject::invokeMethod(manager,
                              "proxyProgress",
                              Qt::QueuedConnection,
                              Q_ARG(QString, clipName),
                              Q_ARG(int, percent));
}

ProxyTranscodeResult runProxyTranscode(const ProxyTranscodeJob &job,
                                       std::atomic_bool &cancelRequested)
{
    ProxyTranscodeResult result;

    QFile::remove(job.proxyPath);

    libavcore::MediaDecoder decoder;
    if (auto err = decoder.open(job.originalPath.toUtf8().constData(), false)) {
        result.error = QString::fromStdString(*err);
        return result;
    }

    const libavcore::VideoStreamProps props = decoder.videoProps();
    const int fps = fpsFromRational(props.frameRate);

    libavcore::EncodeRequest request;
    request.width = job.config.proxyWidth;
    request.height = job.config.proxyHeight;
    request.fps = fps;
    request.videoBitrateBits =
        proxyVideoBitrateBits(job.config, job.preset, fps);
    request.outputPath = job.proxyPath.toUtf8().constData();
    request.audioSourcePath = job.originalPath.toUtf8().constData();
    // Seed the request with the encoder the in-process fallback chain would
    // actually resolve to (h264_mf on Windows, libx264 elsewhere). A bare
    // "libx264" here would be self-contradictory: encoderAvailableHook below
    // reports it unavailable whenever h264_mf is registered. FrameEncoder
    // re-runs its own fallback chain regardless, but starting from the real
    // candidate keeps the request internally consistent and the diagnostics
    // accurate.
    request.videoCodecName =
        inProcessH264FallbackEncoderName().toUtf8().constData();
    request.encoderAvailableHook = proxyEncoderAvailableForInProcess;

    {
        libavcore::FrameEncoder encoder;
        if (auto err = encoder.open(request)) {
            result.error = QString::fromStdString(*err);
            QFile::remove(job.proxyPath);
            return result;
        }
        result.activeEncoderName =
            QString::fromStdString(encoder.activeEncoderName());

        AVFrame *scaledFrame = av_frame_alloc();
        SwsContext *sws = nullptr;
        if (!scaledFrame) {
            result.error = QStringLiteral("Failed to allocate proxy frame");
        } else {
            const AVPixelFormat dstFmt = encoder.outputPixelFormat();
            scaledFrame->format = dstFmt;
            scaledFrame->width = job.config.proxyWidth;
            scaledFrame->height = job.config.proxyHeight;
            int rc = av_frame_get_buffer(scaledFrame, 32);
            if (rc < 0) {
                result.error =
                    QStringLiteral("Failed to allocate proxy frame buffer");
            }

            int64_t nextPts = 0;
            int lastProgress = -1;
            const AVRational usTimeBase{1, AV_TIME_BASE};
            AVFrame *frame = nullptr;

            while (result.error.isEmpty()
                   && (frame = decoder.nextVideoFrame())) {
                if (cancelRequested.load(std::memory_order_acquire)) {
                    result.cancelled = true;
                    break;
                }

                const AVPixelFormat srcFmt =
                    static_cast<AVPixelFormat>(frame->format);
                sws = sws_getCachedContext(sws,
                                           frame->width,
                                           frame->height,
                                           srcFmt,
                                           job.config.proxyWidth,
                                           job.config.proxyHeight,
                                           dstFmt,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
                if (!sws) {
                    result.error =
                        QStringLiteral("Failed to create proxy scaler");
                    break;
                }

                rc = av_frame_make_writable(scaledFrame);
                if (rc < 0) {
                    result.error =
                        QStringLiteral("Proxy frame buffer is not writable");
                    break;
                }

                sws_scale(sws,
                          frame->data,
                          frame->linesize,
                          0,
                          frame->height,
                          scaledFrame->data,
                          scaledFrame->linesize);

                scaledFrame->color_primaries = frame->color_primaries;
                scaledFrame->color_trc = frame->color_trc;
                scaledFrame->colorspace = frame->colorspace;
                scaledFrame->color_range = frame->color_range;

                if (!encoder.pushFrameNative(scaledFrame, nextPts++)) {
                    result.error =
                        QStringLiteral("Encoder rejected proxy frame");
                    break;
                }
                ++result.framesEncoded;

                if (job.durationUs > 0) {
                    int64_t elapsedUs = AV_NOPTS_VALUE;
                    const int64_t ts =
                        (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                            ? frame->best_effort_timestamp
                            : frame->pts;
                    if (ts != AV_NOPTS_VALUE && props.timeBase.num > 0
                        && props.timeBase.den > 0) {
                        elapsedUs = av_rescale_q(ts,
                                                 props.timeBase,
                                                 usTimeBase);
                    } else {
                        elapsedUs = av_rescale_q(result.framesEncoded,
                                                 AVRational{1, fps},
                                                 usTimeBase);
                    }

                    if (elapsedUs != AV_NOPTS_VALUE) {
                        const int percent = std::clamp<int>(
                            static_cast<int>((elapsedUs * 100)
                                             / job.durationUs),
                            0,
                            100);
                        if (percent != lastProgress) {
                            lastProgress = percent;
                            postProxyProgress(&ProxyManager::instance(),
                                              job.clipName,
                                              percent);
                        }
                    }
                }
            }
        }

        if (cancelRequested.load(std::memory_order_acquire))
            result.cancelled = true;

        if (!result.cancelled && result.error.isEmpty()) {
            if (result.framesEncoded <= 0) {
                result.error = QStringLiteral("No video frames decoded");
            } else if (auto err = encoder.finalize()) {
                result.error = QString::fromStdString(*err);
            } else {
                result.success = QFile::exists(job.proxyPath);
            }
        }

        if (sws)
            sws_freeContext(sws);
        av_frame_free(&scaledFrame);
    }

    if (!result.success)
        QFile::remove(job.proxyPath);

    if (!result.success && result.error.isEmpty() && !result.cancelled)
        result.error = QStringLiteral("Proxy encode failed");

    return result;
}
}

ProxyManager &ProxyManager::instance()
{
    static ProxyManager mgr;
    return mgr;
}

ProxyManager::ProxyManager()
{
    QDir().mkpath(proxyDir());
    QSettings prefs("VSimpleEditor", "Preferences");
    m_encoderOverride = prefs.value("proxyEncoderOverride").toString();
    int presetInt = prefs.value("proxyQualityPreset",
                                static_cast<int>(QualityPreset::Medium)).toInt();
    if (presetInt < 0 || presetInt > 2)
        presetInt = static_cast<int>(QualityPreset::Medium);
    m_qualityPreset = static_cast<QualityPreset>(presetInt);
    loadIndex();
}

void ProxyManager::setEncoderOverride(const QString &name)
{
    m_encoderOverride = name;
    QSettings prefs("VSimpleEditor", "Preferences");
    prefs.setValue("proxyEncoderOverride", name);
}

void ProxyManager::setQualityPreset(QualityPreset preset)
{
    m_qualityPreset = preset;
    QSettings prefs("VSimpleEditor", "Preferences");
    prefs.setValue("proxyQualityPreset", static_cast<int>(preset));
}

QString ProxyManager::computeConfigFingerprint(const ProxyConfig &cfg,
                                               const QString &encoder,
                                               QualityPreset preset)
{
    const char *presetTag = (preset == QualityPreset::High) ? "H"
                          : (preset == QualityPreset::Low)  ? "L" : "M";
    return QString("%1|%2|%3x%4").arg(encoder, QString(presetTag),
                                       QString::number(cfg.proxyWidth),
                                       QString::number(cfg.proxyHeight));
}

ProxyManager::~ProxyManager()
{
    if (m_thread) {
        m_cancelRequested.store(true, std::memory_order_release);
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    // At process teardown the event loop is gone, so the worker's queued
    // completion lambda (which normally clears this) will never run. Reset
    // it here so the in-flight guard reflects reality during destruction.
    m_workerInFlight = false;
}

void ProxyManager::setConfig(const ProxyConfig &config)
{
    m_config = config;
}

void ProxyManager::generateProxy(const QString &originalFilePath)
{
    if (!QFile::exists(originalFilePath))
        return;

    const QString key = normalizePath(originalFilePath);
    if (key.isEmpty())
        return;

    if (m_entries.contains(key)) {
        auto status = m_entries[key].status;
        if (status == ProxyStatus::Ready || status == ProxyStatus::Generating)
            return;
    }

    ProxyEntry entry;
    entry.originalPath = key;
    entry.proxyPath = proxyFilePath(key);
    entry.proxySize = QSize(m_config.proxyWidth, m_config.proxyHeight);
    entry.status = ProxyStatus::Generating;
    m_entries[key] = entry;

    m_queue.append(key);
    m_totalQueued = m_queue.size();
    m_completed = 0;

    if (!m_thread)
        processNextInQueue();
}

void ProxyManager::generateAllProxies(const QStringList &filePaths)
{
    m_totalQueued = 0;
    m_completed = 0;

    for (const auto &path : filePaths) {
        if (!QFile::exists(path))
            continue;

        const QString key = normalizePath(path);
        if (key.isEmpty())
            continue;

        if (m_entries.contains(key)) {
            auto status = m_entries[key].status;
            if (status == ProxyStatus::Ready || status == ProxyStatus::Generating)
                continue;
        }

        ProxyEntry entry;
        entry.originalPath = key;
        entry.proxyPath = proxyFilePath(key);
        entry.proxySize = QSize(m_config.proxyWidth, m_config.proxyHeight);
        entry.status = ProxyStatus::Generating;
        m_entries[key] = entry;

        m_queue.append(key);
    }

    m_totalQueued = m_queue.size();
    if (m_totalQueued > 0 && !m_thread)
        processNextInQueue();
}

QString ProxyManager::getProxyPath(const QString &originalPath) const
{
    const QString key = normalizePath(originalPath);
    if (m_proxyMode && !key.isEmpty() && m_entries.contains(key)) {
        const auto &entry = m_entries[key];
        if (entry.status == ProxyStatus::Ready
            && !isEntryStale(entry)
            && QFile::exists(entry.proxyPath))
            return entry.proxyPath;
    }
    return originalPath;
}

bool ProxyManager::hasProxy(const QString &originalPath) const
{
    const QString key = normalizePath(originalPath);
    if (key.isEmpty() || !m_entries.contains(key))
        return false;
    const auto &entry = m_entries[key];
    return entry.status == ProxyStatus::Ready
        && !isEntryStale(entry)
        && QFile::exists(entry.proxyPath);
}

QString ProxyManager::currentEffectiveEncoder() const
{
    Q_UNUSED(m_encoderOverride);
    return inProcessH264FallbackEncoderName();
}

bool ProxyManager::isEntryStale(const ProxyEntry &entry) const
{
    // Legacy entries (pre-2026-04-25) carry no fingerprint. Treat them as
    // not-stale so existing proxies keep working until the user explicitly
    // regenerates them — that pass will populate the fingerprint.
    if (entry.configFingerprint.isEmpty())
        return false;

    const QString currentFp = computeConfigFingerprint(
        m_config, currentEffectiveEncoder(), m_qualityPreset);
    if (entry.configFingerprint != currentFp)
        return true;

    const qint64 srcMtime =
        QFileInfo(entry.originalPath).lastModified().toMSecsSinceEpoch();
    if (entry.sourceMtimeMs > 0 && srcMtime > entry.sourceMtimeMs)
        return true;

    return false;
}

QString ProxyManager::staleReason(const QString &originalPath) const
{
    const QString key = normalizePath(originalPath);
    if (key.isEmpty() || !m_entries.contains(key))
        return QString();
    const ProxyEntry &entry = m_entries[key];
    if (entry.configFingerprint.isEmpty())
        return QString();

    QStringList reasons;
    const QString currentFp = computeConfigFingerprint(
        m_config, currentEffectiveEncoder(), m_qualityPreset);
    if (entry.configFingerprint != currentFp)
        reasons << QStringLiteral("設定が変更されました");

    const qint64 srcMtime =
        QFileInfo(entry.originalPath).lastModified().toMSecsSinceEpoch();
    if (entry.sourceMtimeMs > 0 && srcMtime > entry.sourceMtimeMs)
        reasons << QStringLiteral("ソース動画が更新されました");

    return reasons.join(QStringLiteral(" / "));
}

void ProxyManager::setProxyMode(bool enabled)
{
    m_proxyMode = enabled;
}

void ProxyManager::deleteProxy(const QString &originalPath)
{
    const QString key = normalizePath(originalPath);
    if (key.isEmpty() || !m_entries.contains(key))
        return;

    const auto &entry = m_entries[key];
    QFile::remove(entry.proxyPath);
    m_entries.remove(key);
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
    // User-overridable storage path (US-3). Cache invalidated by
    // setProxyStorageDir() bumping g_proxyDirRevision. Without the cache,
    // every preview frame would pay QSettings + QFileInfo + isDir(), which
    // turned into UI stutter on network-mounted storage (Gemini review,
    // 2026-04-25). proxy_index.json is NOT stored here (see
    // proxyIndexPath()), so swapping storageDir doesn't strand entries.
    const int rev = g_proxyDirRevision.load(std::memory_order_acquire);
    {
        QMutexLocker lock(&g_proxyDirMutex);
        if (rev == g_cachedProxyDirRevision && !g_cachedProxyDir.isEmpty())
            return g_cachedProxyDir;
    }

    QSettings prefs("VSimpleEditor", "Preferences");
    const QString custom = prefs.value("proxyStorageDir").toString();
    QString resolved;
    bool customResolvedOk = false;
    if (!custom.isEmpty()) {
        QFileInfo info(custom);
        if (info.exists() && info.isDir()) {
            resolved = custom;
            customResolvedOk = true;
        }
    }
    if (resolved.isEmpty())
        resolved = QDir::homePath() + "/.veditor/proxies";

    // Don't cache the homePath fallback when the user actually has a
    // custom path configured but it transiently doesn't exist (mid-mkpath,
    // detached drive, etc.) — otherwise we'd pin the fallback for the
    // entire revision and the user's storage would silently stop being
    // honoured even after the directory becomes available again
    // (Gemini v2 review, 2026-04-25).
    const bool safeToCache = custom.isEmpty() || customResolvedOk;
    if (safeToCache) {
        QMutexLocker lock(&g_proxyDirMutex);
        g_cachedProxyDir = resolved;
        g_cachedProxyDirRevision = rev;
    }
    return resolved;
}

void ProxyManager::setProxyStorageDir(const QString &dir)
{
    // Create the target directory BEFORE persisting the setting so that
    // a concurrent proxyDir() can't observe `custom != "" && !exists()`,
    // fall back to the default homePath, and pin that fallback into the
    // cache for the new revision (Gemini v2 review, 2026-04-25). Without
    // this, the user would have to restart or re-open settings before
    // the new path took effect.
    if (!dir.isEmpty())
        QDir().mkpath(dir);

    QSettings prefs("VSimpleEditor", "Preferences");
    prefs.setValue("proxyStorageDir", dir);
    // Release-side bump pairs with the acquire-load in proxyDir(). This
    // only orders the revision counter; the cached QString is separately
    // protected by g_proxyDirMutex.
    g_proxyDirRevision.fetch_add(1, std::memory_order_release);
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

QString ProxyManager::normalizePath(const QString &path)
{
    if (path.isEmpty())
        return QString();

    // Deterministic, I/O-free normalisation. Earlier drafts called
    // QFileInfo::canonicalFilePath() but that has two failure modes the
    // multi-LLM review (Gemini, 2026-04-25) flagged as blockers:
    //   1. canonicalFilePath() does a stat() — getting hit on every
    //      preview frame turned proxyDir()'s I/O cache (US-1) into a
    //      no-op for network drives and spun-down disks.
    //   2. canonicalFilePath() returns empty when the file is gone, so
    //      a temporarily-detached external drive would silently rewrite
    //      the m_entries key from canonical to fallback form, losing
    //      the existing entry.
    // Cleaning the path and lowercasing on Windows is enough to collapse
    // `C:\foo.mp4` and `c:/foo.mp4` onto the same key, which is the
    // duplicate-entry case the original Codex review flagged. Symlink
    // resolution is intentionally dropped — it's rare in editing
    // workflows and not worth the I/O regression.
    QString clean = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    clean = clean.toLower();
#endif
    return clean;
}

QString ProxyManager::hashPath(const QString &path) const
{
    QByteArray data = path.toUtf8();
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString ProxyManager::proxyFilePath(const QString &originalPath) const
{
    // Hash the canonicalised key — callers already canonicalise before
    // passing in, but doing it here too keeps the hash stable for any
    // private-only call site that forgets.
    QString hash = hashPath(normalizePath(originalPath));
    return proxyDir() + "/" + hash + "." + m_config.format;
}

// Returns the canonical path for proxy_index.json, intentionally stored
// OUTSIDE proxyDir(). Rationale (Codex review, 2026-04-25): proxyDir() is
// now user-configurable via QSettings (US-3). If we kept the index inside
// the storage directory, switching the storage path would make the old
// index invisible and every Ready entry would be reported as None. By
// pinning the index to AppLocalDataLocation we keep the registry stable
// across storageDir changes and let proxyPath itself remain absolute.
static QString proxyIndexPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/.veditor";
    QDir().mkpath(base);
    return base + "/proxy_index.json";
}

void ProxyManager::loadIndex()
{
    const QString indexPath = proxyIndexPath();
    // One-shot migration: if the new path doesn't exist but the legacy
    // proxyDir()/index.json does, copy it over and remove the old one.
    if (!QFile::exists(indexPath)) {
        const QString legacyPath = proxyDir() + "/index.json";
        if (QFile::exists(legacyPath)) {
            QFile legacy(legacyPath);
            if (legacy.open(QIODevice::ReadOnly)) {
                const QByteArray content = legacy.readAll();
                legacy.close();
                QFile dst(indexPath);
                if (dst.open(QIODevice::WriteOnly)) {
                    const qint64 written = dst.write(content);
                    dst.close();
                    // Only delete the legacy file once we have proof the
                    // new copy holds the full payload. A short write (e.g.
                    // disk full) would otherwise wipe the index entirely.
                    if (written == content.size())
                        QFile::remove(legacyPath);
                }
            }
        }
    }

    QFile file(indexPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return;

    QJsonObject root = doc.object();
    QJsonArray entries = root["entries"].toArray();

    // Migration (2026-04-25): pre-existing indices keyed entries on the
    // raw user-typed path, so the same source could appear twice under
    // case- or slash-variants. We canonicalise on load and, when two
    // legacy entries collapse onto the same key, keep the one with the
    // newer sourceMtimeMs and drop the loser's proxy file from disk.
    bool migrated = false;
    for (const auto &val : entries) {
        QJsonObject obj = val.toObject();
        ProxyEntry entry;
        const QString rawPath = obj["originalPath"].toString();
        entry.originalPath = normalizePath(rawPath);
        if (entry.originalPath.isEmpty())
            entry.originalPath = rawPath; // best-effort: keep raw if normalize fails
        entry.proxyPath = obj["proxyPath"].toString();
        entry.originalSize = QSize(obj["originalWidth"].toInt(), obj["originalHeight"].toInt());
        entry.proxySize = QSize(obj["proxyWidth"].toInt(), obj["proxyHeight"].toInt());
        // New fields (post-2026-04-25). Old indices missing them → defaults
        // preserve backward compatibility (empty fingerprint / 0 mtime).
        entry.configFingerprint = obj.value("configFingerprint").toString();
        entry.sourceMtimeMs =
            static_cast<qint64>(obj.value("sourceMtimeMs").toDouble(0.0));

        if (QFile::exists(entry.proxyPath))
            entry.status = ProxyStatus::Ready;
        else
            entry.status = ProxyStatus::None;

        if (entry.originalPath != rawPath)
            migrated = true;

        if (m_entries.contains(entry.originalPath)) {
            // Duplicate after canonicalisation. Tie-break order (Codex
            // review, 2026-04-25):
            //   1. Prefer the entry whose proxy file actually exists on
            //      disk and is in Ready state — losing a usable proxy to
            //      a stale Failed/None twin would force pointless
            //      regeneration.
            //   2. If both (or neither) are usable, prefer the newer
            //      source mtime — it tracks which version of the source
            //      the user most recently re-encoded for.
            // Drop the loser's proxy file so we don't strand orphan
            // bytes in the storage dir.
            const ProxyEntry &existing = m_entries[entry.originalPath];
            auto isUsable = [](const ProxyEntry &e) {
                return e.status == ProxyStatus::Ready && QFile::exists(e.proxyPath);
            };
            const bool incomingUsable = isUsable(entry);
            const bool existingUsable = isUsable(existing);
            bool incomingWins;
            if (incomingUsable && !existingUsable)
                incomingWins = true;
            else if (!incomingUsable && existingUsable)
                incomingWins = false;
            else
                incomingWins = entry.sourceMtimeMs > existing.sourceMtimeMs;

            if (incomingWins) {
                if (existing.proxyPath != entry.proxyPath)
                    QFile::remove(existing.proxyPath);
                m_entries[entry.originalPath] = entry;
            } else if (existing.proxyPath != entry.proxyPath) {
                QFile::remove(entry.proxyPath);
            }
            migrated = true;
        } else {
            m_entries[entry.originalPath] = entry;
        }
    }

    // If any keys were rewritten or duplicates merged, persist the
    // canonical form so a future load pass doesn't re-do this work.
    if (migrated)
        saveIndex();
}

void ProxyManager::saveIndex()
{
    const QString indexPath = proxyIndexPath();
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
        obj["configFingerprint"] = entry.configFingerprint;
        obj["sourceMtimeMs"] = static_cast<double>(entry.sourceMtimeMs);
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
    qInfo().noquote() << QStringLiteral("[ProxyManager] cancellation requested by user");
    m_cancelRequested.store(true, std::memory_order_release);
    m_queue.clear();

    // The worker observes m_cancelRequested inside its decode loop. Reset
    // entries immediately so a later generateAllProxies call can queue them
    // again after the worker has marshalled its cancellation completion.
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it.value().status == ProxyStatus::Generating)
            it.value().status = ProxyStatus::None;
    }

    if (m_thread) {
        // Synchronously join the worker before returning. Callers depend on
        // this: MainWindow's regenerate path deletes the proxy file the
        // instant cancelGeneration() returns, and the worker's libavcore
        // FrameEncoder still holds that file open until run() unwinds —
        // unlinking it mid-encode crashes the app. Without the join the
        // worker could also still be alive when the next generateAllProxies
        // arrives, allowing two overlapping transcodes.
        //
        // No deadlock: the worker's run() body finishes runProxyTranscode()
        // (which exits its decode loop as soon as it sees m_cancelRequested)
        // and then only POSTs the completion lambda via Qt::QueuedConnection
        // before returning, so wait() on the main thread unblocks promptly.
        //
        // We deliberately do NOT delete the thread here. The completion
        // lambda was already posted and will run later on the main thread;
        // it calls workerThread->wait() (idempotent — a second wait() on a
        // finished thread is safe) and owns the deleteLater(). Deleting the
        // QThread here would leave that queued lambda dereferencing freed
        // memory. m_thread therefore stays non-null until the lambda runs.
        m_thread->wait();
    }

    // m_cancelRequested is left set while a worker is still in flight; the
    // completion lambda is the single authority that clears it (and clears
    // m_workerInFlight) once it has finished marshalling the cancelled
    // result. Clearing it here would let the flag race a not-yet-delivered
    // completion. When no worker exists there is nothing to wait on, so
    // reset the per-job state immediately.
    if (!m_thread && !m_workerInFlight) {
        m_cancelRequested.store(false, std::memory_order_release);
        m_currentClipName.clear();
        m_currentSourceDurationUs = 0;
    }
}

void ProxyManager::processNextInQueue()
{
    // Refuse to start a second worker while one is still in flight. m_thread
    // is checked too, but it alone is insufficient: the completion lambda
    // nulls m_thread before this function is re-entered, so a re-entrant
    // caller (e.g. generateAllProxies arriving between worker run() returning
    // and the queued completion lambda executing) could see m_thread ==
    // nullptr and start an overlapping transcode. m_workerInFlight stays true
    // for the whole worker lifetime — from start() until the completion
    // lambda clears it — and closes that gap.
    if (m_thread || m_workerInFlight)
        return;

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
    m_cancelRequested.store(false, std::memory_order_release);
    emit proxyStarted(m_currentClipName);
    if (m_currentSourceDurationUs <= 0) {
        // Probe failed or input has no duration (image, broken file). Push
        // an indeterminate marker so the dialog stops sitting at 0%.
        emit proxyProgress(m_currentClipName, -1);
    }

    const QString plannedEncoder = currentEffectiveEncoder();
    const char *presetTag = "M";
    switch (m_qualityPreset) {
        case QualityPreset::High:   presetTag = "H"; break;
        case QualityPreset::Medium: presetTag = "M"; break;
        case QualityPreset::Low:    presetTag = "L"; break;
    }
    // bitrate is an ESTIMATE: the real source fps is only known once the
    // worker opens the decoder, so this main-thread log uses a nominal
    // 30 fps. The encoder still scales the bitrate to the actual fps via
    // proxyVideoBitrateBits() inside runProxyTranscode(); only this
    // diagnostic line is approximate.
    appendEncoderLog(QString("[%1] job source=%2 encoder=%3 decoder=%4 bitrate~=%5(est@30fps) preset=%6")
                     .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                          m_currentClipName,
                          plannedEncoder,
                          srcCodec.isEmpty() ? QStringLiteral("<probe-failed>")
                                             : srcCodec,
                          QString::number(proxyVideoBitrateBits(
                              m_config, m_qualityPreset, 30)),
                          QString::fromLatin1(presetTag)));

    ProxyTranscodeJob job;
    job.originalPath = originalPath;
    job.proxyPath = entry.proxyPath;
    job.clipName = m_currentClipName;
    job.config = m_config;
    job.preset = m_qualityPreset;
    job.durationUs = m_currentSourceDurationUs;

    // Mark the worker in flight BEFORE start() so any re-entrant
    // processNextInQueue() (or a generateAllProxies racing the queued
    // completion lambda) is structurally blocked from launching a second
    // transcode. Cleared only by the completion lambda below.
    m_workerInFlight = true;

    m_thread = QThread::create([this, originalPath, job]() {
        ProxyTranscodeResult result =
            runProxyTranscode(job, m_cancelRequested);
        QThread *workerThread = QThread::currentThread();
        QMetaObject::invokeMethod(
            this,
            [this, originalPath, job, result, workerThread]() {
                if (m_thread == workerThread) {
                    workerThread->wait();
                    m_thread = nullptr;
                    workerThread->deleteLater();
                }
                // The worker is fully retired (run() returned, thread
                // joined). Drop the in-flight guard now so the
                // processNextInQueue() call below — and any external caller
                // — can launch the next job.
                m_workerInFlight = false;

                const bool wasCancelled =
                    result.cancelled
                    || m_cancelRequested.load(std::memory_order_acquire);
                const bool success = result.success && !wasCancelled;
                const QString clipName = job.clipName;
                const QString activeEncoder =
                    result.activeEncoderName.isEmpty()
                        ? currentEffectiveEncoder()
                        : result.activeEncoderName;

                if (m_entries.contains(originalPath)) {
                    auto &entry = m_entries[originalPath];
                    if (success && QFile::exists(entry.proxyPath)) {
                        entry.status = ProxyStatus::Ready;
                        entry.configFingerprint = computeConfigFingerprint(
                            m_config, activeEncoder, m_qualityPreset);
                        entry.sourceMtimeMs =
                            QFileInfo(originalPath).lastModified()
                                .toMSecsSinceEpoch();
                        saveIndex();
                        emit proxyGenerated(originalPath, entry.proxyPath);
                    } else {
                        entry.status = wasCancelled ? ProxyStatus::None
                                                    : ProxyStatus::Error;
                        QFile::remove(entry.proxyPath);
                    }
                }

                m_completed++;
                if (m_totalQueued > 0) {
                    int percent =
                        static_cast<int>(m_completed * 100 / m_totalQueued);
                    emit progressChanged(percent);
                }

                if (wasCancelled)
                    emit proxyCancelled(clipName);
                else
                    emit proxyFinished(clipName, success);

                if (!success && !wasCancelled && !result.error.isEmpty()) {
                    appendEncoderLog(QString("[%1] failed source=%2 error=%3")
                        .arg(QDateTime::currentDateTimeUtc()
                                 .toString(Qt::ISODate),
                             clipName,
                             result.error));
                }

                m_currentClipName.clear();
                m_currentSourceDurationUs = 0;
                m_cancelRequested.store(false, std::memory_order_release);

                if (wasCancelled) {
                    if (!m_queue.isEmpty())
                        processNextInQueue();
                    return;
                }

                processNextInQueue();
            },
            Qt::QueuedConnection);
    });
    m_thread->start();
}

bool ProxyManager::ffmpegHasEncoder(const QString &encoderName)
{
    static QHash<QString, bool> cache;
    static QMutex mutex;

    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(encoderName);
        if (it != cache.constEnd())
            return it.value();
    }

    const bool has =
        libavcore::encoderAvailable(encoderName.toStdString());
    {
        QMutexLocker lock(&mutex);
        cache.insert(encoderName, has);
    }
    return has;
}

bool ProxyManager::ffmpegHasDecoder(const QString &decoderName)
{
    static QHash<QString, bool> cache;
    static QMutex mutex;

    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(decoderName);
        if (it != cache.constEnd())
            return it.value();
    }

    const bool has =
        libavcore::decoderAvailable(decoderName.toStdString());
    {
        QMutexLocker lock(&mutex);
        cache.insert(decoderName, has);
    }
    return has;
}

qint64 ProxyManager::probeDurationUs(const QString &path)
{
    static QHash<QString, qint64> cache;
    static QMutex mutex;

    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(path);
        if (it != cache.constEnd())
            return it.value();
    }

    const std::string pathUtf8 = path.toUtf8().constData();
    const std::optional<int64_t> probed =
        libavcore::probeDurationMicroseconds(pathUtf8);
    const qint64 us = (probed && *probed > 0)
        ? static_cast<qint64>(*probed)
        : 0;
    {
        QMutexLocker lock(&mutex);
        cache.insert(path, us);
    }
    return us;
}

QString ProxyManager::probeSourceCodec(const QString &path)
{
    static QHash<QString, QString> cache;
    static QMutex mutex;

    {
        QMutexLocker lock(&mutex);
        auto it = cache.constFind(path);
        if (it != cache.constEnd())
            return it.value();
    }

    const std::string pathUtf8 = path.toUtf8().constData();
    const std::optional<std::string> probed =
        libavcore::probeVideoCodecName(pathUtf8);
    const QString codec = probed
        ? QString::fromStdString(*probed).toLower()
        : QString();
    {
        QMutexLocker lock(&mutex);
        cache.insert(path, codec);
    }
    return codec;
}

void ProxyManager::appendEncoderLog(const QString &line)
{
    // Pin the encoder log next to proxy_index.json (AppLocalDataLocation),
    // not under proxyDir(). Otherwise switching the user-configurable
    // storageDir would fragment diagnostic history across folders and
    // hide older entries from anyone tailing the same file.
    const QString logPath =
        QFileInfo(proxyIndexPath()).absolutePath() + "/encoder_log.txt";
    QFile f(logPath);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << line << '\n';
}
