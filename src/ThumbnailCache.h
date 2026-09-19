#pragma once

#include <QObject>
#include <QCache>
#include <QImage>
#include <QString>
#include <QSize>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <atomic>
#include <memory>

// All public operations belong to the object's (UI) thread. Workers own only
// copied requests and cancellation tokens, so destruction never waits on decode.
class ThumbnailCache : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailCache(QObject *parent = nullptr, int maxBytes = 64 * 1024 * 1024);
    ~ThumbnailCache() override;
    void request(const QString &key, const QString &filePath, double durationSec,
                 int count = 8, QSize size = QSize(128, 72));
    QVector<QImage> frames(const QString &key);
    double duration(const QString &key) const;
    void clear();
    quint64 requestCount() const { return m_requestCount; }
    int activeCount() const { return m_active; }
    int memoryBytes() const { return m_cache.totalCost(); }
    static int skimIndex(double x, double width, int count);

signals:
    void ready(const QString &key, const QVector<QImage> &frames);

private:
    struct Request {
        QString key, path;
        double duration;
        int count;
        QSize size;
    };
    void startNext();
    struct Result { QVector<QImage> frames; double duration = 0.0; };
    QCache<QString, Result> m_cache;
    QQueue<Request> m_queue;
    QSet<QString> m_pending;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_active = 0;
    quint64 m_requestCount = 0;
};
