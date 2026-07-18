#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QtGlobal>

namespace playback {

struct CompositeFrameKey {
    quint64 timelineRevision;
    qint64  timeMs;
    int     width;
    int     height;
    int     qualityTier;
    bool    useProxy;

    bool operator==(const CompositeFrameKey& other) const noexcept
    {
        return timelineRevision == other.timelineRevision
            && timeMs           == other.timeMs
            && width            == other.width
            && height           == other.height
            && qualityTier      == other.qualityTier
            && useProxy         == other.useProxy;
    }
};

inline size_t qHash(const CompositeFrameKey& key, size_t seed = 0) noexcept
{
    seed = ::qHash(key.timelineRevision, seed);
    seed = ::qHash(key.timeMs,           seed);
    seed = ::qHash(key.width,            seed);
    seed = ::qHash(key.height,           seed);
    seed = ::qHash(key.qualityTier,      seed);
    seed = ::qHash(static_cast<int>(key.useProxy), seed);
    return seed;
}

class CompositeFrameCache {
public:
    struct Stats {
        qint64 hits;
        qint64 misses;
        qint64 bytes;
        int    count;
    };

    explicit CompositeFrameCache();

    // Set memory budget in bytes. Default ~256 MiB.
    // Setting to 0 disables caching entirely (put becomes no-op, tryGet always misses).
    void setMemoryBudgetBytes(qint64 bytes);

    // Returns true on hit and fills outImage; promotes entry to most-recently-used.
    // Returns false on miss.
    bool tryGet(const CompositeFrameKey& key, QImage& outImage);

    // Inserts image into cache. Evicts LRU entries until budget is satisfied.
    // If a single image exceeds the budget it is silently dropped (graceful).
    void put(const CompositeFrameKey& key, const QImage& image);

    // Removes all entries whose timelineRevision differs from currentRevision.
    void invalidateRevision(quint64 currentRevision);

    // Removes all entries.
    void clear();

    // Returns a snapshot of current cache statistics.
    Stats stats() const;

private:
    void evictToFit(qint64 neededBytes);  // caller must hold m_mutex

    mutable QMutex              m_mutex;
    qint64                      m_budgetBytes;

    // LRU order: front = least-recently-used, back = most-recently-used.
    QList<CompositeFrameKey>            m_lruOrder;
    QHash<CompositeFrameKey, QImage>    m_cache;

    // Stats
    qint64 m_hits;
    qint64 m_misses;
    qint64 m_currentBytes;
};

} // namespace playback
