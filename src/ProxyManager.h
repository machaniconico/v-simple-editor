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

private:
    ProxyManager();
    ~ProxyManager();

    QString hashPath(const QString &path) const;
    QString proxyFilePath(const QString &originalPath) const;
    void loadIndex();
    void saveIndex();
    void processNextInQueue();

    ProxyConfig m_config;
    bool m_proxyMode = true;
    QHash<QString, ProxyEntry> m_entries;   // originalPath -> entry
    QStringList m_queue;
    int m_totalQueued = 0;
    int m_completed = 0;
    QProcess *m_process = nullptr;
    QThread *m_thread = nullptr;
};
