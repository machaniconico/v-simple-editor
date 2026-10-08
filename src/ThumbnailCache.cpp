#include "ThumbnailCache.h"
#include "libavcore/FrameGrab.h"
#include "libavcore/Probe.h"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

ThumbnailCache::ThumbnailCache(QObject *parent, int maxBytes)
    : QObject(parent), m_cache(qMax(0, maxBytes)),
      m_cancel(std::make_shared<std::atomic_bool>(false))
{
}

ThumbnailCache::~ThumbnailCache()
{
    m_cancel->store(true);
}

int ThumbnailCache::skimIndex(double x, double width, int count)
{
    if (count <= 1 || width <= 0 || !std::isfinite(width) || !std::isfinite(x))
        return 0;
    return static_cast<int>(qBound(0.0, x / width * count, double(count - 1)));
}

QVector<QImage> ThumbnailCache::frames(const QString &key)
{
    const auto *value = m_cache.object(key); // QCache access updates LRU order.
    return value ? value->frames : QVector<QImage>();
}

double ThumbnailCache::duration(const QString &key) const
{
    const auto *value = m_cache.object(key);
    return value ? value->duration : 0.0;
}

void ThumbnailCache::request(const QString &key, const QString &filePath,
                             double durationSec, int count, QSize size)
{
    if (key.isEmpty() || filePath.isEmpty() || m_pending.contains(key) || m_cache.contains(key))
        return;
    ++m_requestCount;
    m_pending.insert(key);
    m_queue.enqueue({key, filePath, std::isfinite(durationSec) ? qMax(0.0, durationSec) : 0.0,
                     qBound(1, count, 256), size.isValid() && !size.isEmpty() ? size : QSize(128, 72)});
    startNext();
}

void ThumbnailCache::clear()
{
    m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_queue.clear();
    m_pending.clear();
    m_cache.clear();
    // Cancelled workers still occupy slots until finished: never exceed two.
}

void ThumbnailCache::startNext()
{
    while (m_active < 2 && !m_queue.isEmpty()) {
        const Request request = m_queue.dequeue();
        const auto cancel = m_cancel;
        ++m_active;
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this,
                [this, watcher, request, cancel] {
            const auto result = watcher->result();
            watcher->deleteLater();
            --m_active;
            if (!cancel->load()) {
                m_pending.remove(request.key);
                qint64 bytes = 1; // Also cache failed/unsupported sources.
                for (const auto &frame : result.frames)
                    bytes += frame.sizeInBytes();
                if (bytes <= m_cache.maxCost())
                    m_cache.insert(request.key, new Result(result), static_cast<int>(bytes));
                emit ready(request.key, result.frames);
            }
            startNext();
        });
        watcher->setFuture(QtConcurrent::run([request, cancel] {
            Result result;
            const bool stillImage = libavcore::isStillImage(request.path);
            const int count = stillImage ? 1 : request.count;
            double duration = request.duration;
            // Imported assets may not have duration metadata. Probe only here,
            // on the worker, so importing and hovering never block the UI.
            if (!stillImage && duration <= 0.0 && !cancel->load()) {
                const auto microseconds = libavcore::probeDurationMicroseconds(request.path.toStdString());
                if (microseconds && *microseconds > 0)
                    duration = *microseconds / 1000000.0;
            }
            result.duration = duration;
            for (int i = 0; i < count && !cancel->load(); ++i) {
                QImage image = libavcore::grabFrameAt(request.path,
                    duration * i / count, request.size);
                if (image.isNull())
                    return Result();
                result.frames.append(image);
            }
            return cancel->load() ? Result() : result;
        }));
    }
}
