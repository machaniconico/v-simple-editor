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

    // Manual encoder override. Empty string = Auto (existing behaviour:
    // chosenGpuH264Encoder() priority chain). Otherwise one of
    //   "h264_nvenc" / "h264_qsv" / "h264_amf" / "libx264"
    // Persisted via QSettings("VSimpleEditor", "Preferences") under
    // "proxyEncoderOverride".
    QString encoderOverride() const { return m_encoderOverride; }
    void setEncoderOverride(const QString &name);

    // Quality preset (US-2). Persisted via QSettings under
    // "proxyQualityPreset" as int (0/1/2). Mapping to encoder-specific
    // CRF/QP values lives in qualityValueForEncoder.
    QualityPreset qualityPreset() const { return m_qualityPreset; }
    void setQualityPreset(QualityPreset preset);

    // Probe the runtime ffmpeg.exe for the named encoder. Public so the
    // settings dialog can grey out unsupported items. Cached internally.
    static bool ffmpegHasEncoder(const QString &encoderName);

    // Read-only access to the entry registry for the management dialog.
    const QHash<QString, ProxyEntry> &entries() const { return m_entries; }

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

    // Mirror of ffmpegHasEncoder for the decoder side. Parses the output of
    // `ffmpeg -hide_banner -decoders` so we can verify a hardware decoder
    // (e.g. av1_cuvid, h264_qsv) is actually registered before we hand it to
    // the encoder branch. Cached per name for the singleton lifetime.
    static bool ffmpegHasDecoder(const QString &decoderName);

    // Spawn ffprobe to read the source file duration in microseconds. Used
    // to convert ffmpeg's out_time_ms into a percentage for proxyProgress.
    // Returns 0 on any probe failure (start failed, timeout, non-zero exit,
    // unparseable output) — callers treat 0 as "duration unknown" and emit
    // proxyProgress(name, -1) so the dialog stays in indeterminate mode.
    // Result is cached per path for the lifetime of the singleton because
    // the same source can be re-queued and ffprobe spawn is ~200 ms.
    static qint64 probeDurationUs(const QString &path);

    // Spawn ffprobe to read the source's first video stream codec name
    // (e.g. "av1", "h264", "hevc", "vp9"). Used by the GPU encoder branches
    // to inject the matching cuvid/qsv decoder before -i so the input is
    // hardware-decoded instead of falling back to libdav1d / libx264 etc.
    // Returns an empty QString on any probe failure or when ffprobe is not
    // on PATH; callers should treat empty as "skip decoder injection and let
    // ffmpeg pick the default". Result is cached per path.
    static QString probeSourceCodec(const QString &path);

    // Probe the runtime ffmpeg.exe for the first available GPU H.264 encoder
    // in the order h264_nvenc → h264_qsv → h264_amf (typical throughput
    // ranking on consumer hardware). Returns the encoder name or an empty
    // string when no GPU encoder is registered. Cached for the singleton
    // lifetime — encoders don't appear at runtime.
    static QString chosenGpuH264Encoder();

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
    bool m_cancelRequested = false;

    ProxyConfig m_config;
    bool m_proxyMode = true;
    QHash<QString, ProxyEntry> m_entries;   // originalPath -> entry
    QStringList m_queue;
    int m_totalQueued = 0;
    int m_completed = 0;
    QProcess *m_process = nullptr;
    QThread *m_thread = nullptr;

    QString m_encoderOverride;
    QualityPreset m_qualityPreset = QualityPreset::Medium;

    // Map (encoder, preset) → CRF/QP value used in the corresponding ffmpeg
    // arg branch. Tables are calibrated per-backend because libx264 CRF and
    // h264_qsv global_quality scales aren't equivalent.
    static int qualityValueForEncoder(const QString &encoder, QualityPreset preset);
};
