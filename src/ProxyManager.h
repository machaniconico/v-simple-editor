#pragma once

#include <QObject>
#include <QString>
#include <QSize>
#include <QHash>
#include <QStringList>
#include <QProcess>
#include <QThread>

enum class ProxyStatus {
    None,
    Generating,
    Ready,
    Error
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
};

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
    bool hasProxy(const QString &originalPath) const;

    bool isProxyMode() const { return m_proxyMode; }
    void setProxyMode(bool enabled);

    void deleteProxy(const QString &originalPath);
    void deleteAllProxies();

    static QString proxyDir();
    qint64 diskUsage() const;

signals:
    void proxyGenerated(const QString &originalPath, const QString &proxyPath);
    void progressChanged(int percent);
    void allProxiesReady();

    // Per-clip progress signals consumed by ProxyProgressDialog.
    // proxyStarted: emitted right before ffmpeg launches for a clip.
    // proxyProgress: percent is parsed from ffmpeg's `out_time_ms` against
    //   the source duration (best-effort; -1 if duration is unknown).
    // proxyFinished: ok=true on success, false on error or user cancel.
    // proxyCancelled: distinct from proxyFinished(ok=false) so the UI can
    //   distinguish user-initiated abort from a real failure.
    void proxyStarted(const QString &clipName);
    void proxyProgress(const QString &clipName, int percent);
    void proxyFinished(const QString &clipName, bool ok);
    void proxyCancelled(const QString &clipName);

public slots:
    // Terminate the in-flight ffmpeg process and clear the queue. Safe to
    // call when no generation is active (no-op).
    void cancelGeneration();

private:
    ProxyManager();
    ~ProxyManager();

    QString hashPath(const QString &path) const;
    QString proxyFilePath(const QString &originalPath) const;
    void loadIndex();
    void saveIndex();
    void processNextInQueue();
    void parseFfmpegProgress(const QByteArray &chunk);

    // Probe the runtime ffmpeg.exe (the binary we'll actually invoke) for an
    // encoder by parsing `ffmpeg -hide_banner -encoders` output. This is
    // *different* from CodecDetector::isEncoderAvailable — that one queries
    // the linked libavcodec, which can disagree with the PATH ffmpeg when
    // they were built independently. Result is cached per encoder name for
    // the lifetime of the singleton (encoders never appear/disappear at
    // runtime).
    static bool ffmpegHasEncoder(const QString &encoderName);

    // Spawn ffprobe to read the source file duration in microseconds. Used
    // to convert ffmpeg's out_time_ms into a percentage for proxyProgress.
    // Returns 0 on any probe failure (start failed, timeout, non-zero exit,
    // unparseable output) — callers treat 0 as "duration unknown" and emit
    // proxyProgress(name, -1) so the dialog stays in indeterminate mode.
    // Result is cached per path for the lifetime of the singleton because
    // the same source can be re-queued and ffprobe spawn is ~200 ms.
    static qint64 probeDurationUs(const QString &path);

    // Probe the runtime ffmpeg.exe for the first available GPU H.264 encoder
    // in the order h264_nvenc → h264_qsv → h264_amf (typical throughput
    // ranking on consumer hardware). Returns the encoder name or an empty
    // string when no GPU encoder is registered. Cached for the singleton
    // lifetime — encoders don't appear at runtime.
    static QString chosenGpuH264Encoder();

    // Source duration in microseconds for the in-flight proxy job, populated
    // by probeDurationUs at queue time. Zero means probe failed or input has
    // no duration (image-only) — see probeDurationUs for the contract.
    qint64 m_currentSourceDurationUs = 0;
    QString m_currentClipName;
    bool m_cancelRequested = false;

    ProxyConfig m_config;
    bool m_proxyMode = true;
    QHash<QString, ProxyEntry> m_entries;   // originalPath -> entry
    QStringList m_queue;
    int m_totalQueued = 0;
    int m_completed = 0;
    QProcess *m_process = nullptr;
    QThread *m_thread = nullptr;
};
