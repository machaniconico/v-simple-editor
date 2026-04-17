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

// Stage 2.8 — Premiere/DaVinci/Final Cut all transcode to an intermediate
// codec for editing. H264 is the default compromise (small files, universal
// HW decode); ProResLT/DNxHR are I-frame only so scrubbing and seeking are
// basically free on the CPU at the cost of ~5x file size.
enum class ProxyCodec {
    H264,
    ProResLT,
    DNxHRLB
};

struct ProxyConfig {
    // Stage 2.7 — bumped from 640x360 so editing a 4K source still produces
    // a visually usable proxy. Height is recomputed by ffmpeg (scale=W:-2)
    // to preserve the source aspect ratio and dodge squashed non-16:9 clips.
    ProxyCodec codec = ProxyCodec::H264;
    int proxyWidth = 1920;
    int proxyHeight = 1080;
    int quality = 23;          // CRF value (H.264) / qscale (ProRes) — codec-specific
    QString format = "mp4";    // Refreshed per-codec in processNextInQueue
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

private:
    ProxyManager();
    ~ProxyManager();

    QString hashPath(const QString &path) const;
    QString proxyFilePath(const QString &originalPath) const;
    void loadIndex();
    void saveIndex();
    void processNextInQueue();
    // Stage 2.8.1 — probe ffmpeg -encoders once to see which HW encoders
    // are available on this box. Populates m_hwEncoder with the first
    // winner (nvenc > qsv > amf) or leaves it empty for libx264 fallback.
    void detectHwEncoder();

    ProxyConfig m_config;
    bool m_proxyMode = true;
    QHash<QString, ProxyEntry> m_entries;   // originalPath -> entry
    QStringList m_queue;
    int m_totalQueued = 0;
    int m_completed = 0;
    QProcess *m_process = nullptr;
    QThread *m_thread = nullptr;
    QString m_hwEncoder;                    // e.g. "h264_nvenc", "h264_qsv", "h264_amf"; empty = SW
};
