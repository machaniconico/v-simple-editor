#pragma once

#include <atomic>

#include <QObject>
#include <QString>
#include <QSize>
#include <QHash>
#include <QStringList>
#include <QThread>

enum class ProxyStatus {
    None,
    Generating,
    Ready,
    Error,
    Stale
};

enum class QualityPreset {
    High = 0,
    Medium = 1,
    Low = 2
};

struct ProxyConfig {
    int proxyWidth = 640;
    int proxyHeight = 360;
    int quality = 23;          // CRF value
    QString format = "mp4";
    bool enabled = true;
};

struct ProxyEntry {
    QString originalPath;
    QString proxyPath;
    QSize originalSize;
    QSize proxySize;
    ProxyStatus status = ProxyStatus::None;
    QString configFingerprint;   // empty for legacy entries (pre-2026-04-25 indices)
    qint64 sourceMtimeMs = 0;    // 0 for legacy
};

// Thread-safety contract:
//   - Static helpers (proxyDir, setProxyStorageDir, normalizePath,
//     ffmpegHasEncoder, ffmpegHasDecoder, probeDurationUs,
//     probeSourceCodec) protect their own state
//     and are safe to call from any thread.
//   - All non-static methods (generateProxy, getProxyPath, hasProxy,
//     deleteProxy, isEntryStale, …) and the m_* members they touch
//     (m_entries, m_queue, m_thread, …) are MAIN-THREAD ONLY. The
//     in-process transcoder runs on a worker QThread and must marshal
//     all entry updates and signals back through queued invocations.
//     m_cancelRequested is the only worker-readable member.
class ProxyManager : public QObject
{
    Q_OBJECT

public:
    static ProxyManager &instance();

    void setConfig(const ProxyConfig &config);
    ProxyConfig config() const { return m_config; }

    void generateProxy(const QString &originalFilePath);
    void generateAllProxies(const QStringList &filePaths);

    QString getProxyPath(const QString &originalPath) const;
    QString getProxyPath(const QString &originalPath, bool forceProxy) const;
    bool hasProxy(const QString &originalPath) const;
    bool isGenerating(const QString &originalPath) const;

    bool isProxyMode() const { return m_proxyMode; }
    void setProxyMode(bool enabled);

    void deleteProxy(const QString &originalPath);
    void deleteAllProxies();

    static QString proxyDir();
    // Update the user-configurable proxy storage directory and bump the
    // internal revision so the next proxyDir() call recomputes instead of
    // serving its cached value. All call sites that change
    // QSettings("proxyStorageDir") MUST go through here — a direct
    // setValue() leaves the cache stale.
    static void setProxyStorageDir(const QString &dir);
    qint64 diskUsage() const;

    // Manual encoder override. Kept for settings compatibility. The
    // in-process proxy path ignores disabled GPU choices and lets
    // libavcore's H.264 fallback chain resolve the active encoder. Empty
    // string = Auto. Otherwise one of
    //   "h264_nvenc" / "h264_qsv" / "h264_amf" / "libx264"
    // Persisted via QSettings("VSimpleEditor", "Preferences") under
    // "proxyEncoderOverride".
    QString encoderOverride() const { return m_encoderOverride; }
    void setEncoderOverride(const QString &name);

    // Quality preset (US-2). Persisted via QSettings under
    // "proxyQualityPreset" as int (0/1/2).
    QualityPreset qualityPreset() const { return m_qualityPreset; }
    void setQualityPreset(QualityPreset preset);

    // Legacy-named helper for settings UI compatibility. Probes the linked
    // libavcodec registry through libavcore::Probe; no subprocess is spawned.
    static bool ffmpegHasEncoder(const QString &encoderName);

    // Read the source's first video stream codec name via libavcore::Probe.
    // Returns an empty QString on any probe failure. Thread-safe (static
    // helper, protects its own state). Exposed publicly so the auto-proxy
    // policy (multitrack playback) can classify clips by codec. Synchronous —
    // callers must cache results to avoid repeated probes on the UI thread.
    static QString probeSourceCodec(const QString &path);

    // Read-only access to the entry registry for the management dialog.
    const QHash<QString, ProxyEntry> &entries() const { return m_entries; }

    // True iff entry was generated under a different (encoder, preset, size)
    // tuple OR the source file has been modified since the proxy was made.
    // Legacy entries (empty fingerprint) always return false for back-compat.
    bool isEntryStale(const ProxyEntry &entry) const;

    // Human-readable reason joined by " / " when both apply, or empty if not
    // stale. Used by the management dialog tooltip.
    QString staleReason(const QString &originalPath) const;

signals:
    void proxyGenerated(const QString &originalPath, const QString &proxyPath);
    void progressChanged(int percent);
    void allProxiesReady();

    // Per-clip progress signals consumed by ProxyProgressDialog.
    // proxyStarted: emitted right before in-process transcode begins.
    // proxyProgress: percent is estimated from decoded video timestamps
    //   against the source duration (best-effort; -1 if duration is unknown).
    // proxyFinished: ok=true on success, false on error or user cancel.
    // proxyCancelled: distinct from proxyFinished(ok=false) so the UI can
    //   distinguish user-initiated abort from a real failure.
    void proxyStarted(const QString &clipName);
    void proxyProgress(const QString &clipName, int percent);
    void proxyFinished(const QString &clipName, bool ok);
    void proxyCancelled(const QString &clipName);

public slots:
    // Request cancellation of the in-flight in-process transcode and clear
    // the queue. Safe to call when no generation is active (no-op).
    void cancelGeneration();

private:
    ProxyManager();
    ~ProxyManager();

    QString hashPath(const QString &path) const;
    QString proxyFilePath(const QString &originalPath) const;
    void loadIndex();
    void saveIndex();

    // Canonicalize originalPath so the same source file always lands under
    // the same m_entries key regardless of how the user typed it. On
    // Windows the OS treats `C:\foo.mp4` and `c:/foo.mp4` as the same
    // file but Qt's QString comparison doesn't, which used to leak
    // duplicate entries (and duplicate proxy files) for one source.
    // Returns the canonical absolute path when the file exists; falls
    // back to a clean+lowercased path so even pre-existing-file lookups
    // converge. Empty input returns empty.
    static QString normalizePath(const QString &path);

    // Encoder that the next proxy job would use given the current settings.
    // Mirrors the in-process H.264 fallback chain so isEntryStale can predict
    // the fingerprint without re-running it.
    QString currentEffectiveEncoder() const;
    void processNextInQueue();

    // Legacy-named helper for settings UI compatibility (mirror of
    // ffmpegHasEncoder for the decoder side). Probes the linked libavcodec
    // registry through libavcore::Probe; no subprocess is spawned.
    static bool ffmpegHasDecoder(const QString &decoderName);

    // Read the source file duration in microseconds via libavcore::Probe.
    // Returns 0 on any probe failure; callers treat 0 as "duration unknown"
    // and emit proxyProgress(name, -1) so the dialog stays indeterminate.
    static qint64 probeDurationUs(const QString &path);

    // Append a single line to ~/.veditor/proxies/encoder_log.txt for
    // diagnostic visibility into which encoder was picked and which clips
    // ran through it. Failures are silently ignored — proxy generation
    // never blocks on the diagnostic side-channel.
    static void appendEncoderLog(const QString &line);

    // Source duration in microseconds for the in-flight proxy job, populated
    // by probeDurationUs at queue time. Zero means probe failed or input has
    // no duration (image-only) — see probeDurationUs for the contract.
    qint64 m_currentSourceDurationUs = 0;
    QString m_currentClipName;
    std::atomic_bool m_cancelRequested{false};

    // Explicit in-flight guard, distinct from m_thread. m_thread alone is not
    // a reliable "a worker is running" signal: the completion lambda nulls
    // m_thread *before* the next processNextInQueue() call, so a re-entrant
    // path could observe m_thread == nullptr while a worker is still being
    // torn down. processNextInQueue() refuses to start a second worker while
    // this is true; the completion lambda clears it. Main-thread only.
    bool m_workerInFlight = false;

    ProxyConfig m_config;
    bool m_proxyMode = true;
    QHash<QString, ProxyEntry> m_entries;   // originalPath -> entry
    QStringList m_queue;
    int m_totalQueued = 0;
    int m_completed = 0;
    QThread *m_thread = nullptr;

    QString m_encoderOverride;
    QualityPreset m_qualityPreset = QualityPreset::Medium;

    // Stable, human-readable identifier for the (encoder, preset, size) tuple
    // a proxy was generated under. Mismatch against the current effective
    // config means the proxy is stale and should be regenerated.
    static QString computeConfigFingerprint(const ProxyConfig &cfg,
                                            const QString &encoder,
                                            QualityPreset preset);
};
