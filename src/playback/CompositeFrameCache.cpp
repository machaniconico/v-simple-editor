#include "CompositeFrameCache.h"

#include <QMutexLocker>
#include <utility>

namespace playback {

static constexpr qint64 kDefaultBudgetBytes = 256LL * 1024 * 1024; // 256 MiB

CompositeFrameCache::CompositeFrameCache()
    : m_budgetBytes(kDefaultBudgetBytes)
    , m_hits(0)
    , m_misses(0)
    , m_currentBytes(0)
{
}

void CompositeFrameCache::setMemoryBudgetBytes(qint64 bytes)
{
    QMutexLocker locker(&m_mutex);
    m_budgetBytes = bytes < 0 ? 0 : bytes;
    if (m_budgetBytes == 0) {
        // Budget disabled — discard everything.
        m_lruOrder.clear();
        m_cache.clear();
        m_currentBytes = 0;
    } else {
        evictToFit(0);
    }
}

bool CompositeFrameCache::tryGet(const CompositeFrameKey& key, QImage& outImage)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_cache.find(key);
    if (it == m_cache.end()) {
        ++m_misses;
        return false;
    }

    // Promote to most-recently-used (move to back of lruOrder).
    m_lruOrder.removeOne(key);
    m_lruOrder.append(key);

    outImage = it.value();
    ++m_hits;
    return true;
}

void CompositeFrameCache::put(const CompositeFrameKey& key, const QImage& image)
{
    QMutexLocker locker(&m_mutex);

    if (m_budgetBytes == 0 || image.isNull()) {
        return; // caching disabled
    }

    const qint64 frameBytes = static_cast<qint64>(image.sizeInBytes());
    if (frameBytes <= 0) {
        return;
    }
    if (frameBytes > m_budgetBytes) {
        // Single frame exceeds the entire budget — silently drop.
        return;
    }

    // If already cached, remove old entry first to re-insert as most-recent.
    if (m_cache.contains(key)) {
        const qint64 oldBytes = static_cast<qint64>(m_cache.value(key).sizeInBytes());
        m_cache.remove(key);
        m_lruOrder.removeOne(key);
        m_currentBytes -= oldBytes;
    }

    // Evict LRU entries until there is room.
    evictToFit(frameBytes);
    if (m_currentBytes > m_budgetBytes - frameBytes) {
        return;
    }

    m_cache.insert(key, image);
    m_lruOrder.append(key);
    m_currentBytes += frameBytes;
}

void CompositeFrameCache::invalidateRevision(quint64 currentRevision)
{
    QMutexLocker locker(&m_mutex);

    // Collect keys that belong to a different revision.
    QList<CompositeFrameKey> toRemove;
    for (const CompositeFrameKey& k : std::as_const(m_lruOrder)) {
        if (k.timelineRevision != currentRevision) {
            toRemove.append(k);
        }
    }

    for (const CompositeFrameKey& k : std::as_const(toRemove)) {
        const qint64 bytes = static_cast<qint64>(m_cache.value(k).sizeInBytes());
        m_cache.remove(k);
        m_lruOrder.removeOne(k);
        m_currentBytes -= bytes;
    }
    if (m_currentBytes < 0) {
        m_currentBytes = 0; // guard against underflow
    }
}

void CompositeFrameCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_lruOrder.clear();
    m_cache.clear();
    m_currentBytes = 0;
}

CompositeFrameCache::Stats CompositeFrameCache::stats() const
{
    QMutexLocker locker(&m_mutex);
    return Stats{
        m_hits,
        m_misses,
        m_currentBytes,
        static_cast<int>(m_cache.size())
    };
}

// ---------------------------------------------------------------------------
// Private helpers (caller must hold m_mutex)
// ---------------------------------------------------------------------------

void CompositeFrameCache::evictToFit(qint64 neededBytes)
{
    // Evict from the front (LRU) until budget accommodates neededBytes.
    const qint64 safeNeeded = qMax<qint64>(0, neededBytes);
    const qint64 targetBytes = (safeNeeded >= m_budgetBytes)
        ? 0
        : (m_budgetBytes - safeNeeded);
    while (!m_lruOrder.isEmpty()
           && m_currentBytes > targetBytes)
    {
        const CompositeFrameKey lruKey = m_lruOrder.front();
        m_lruOrder.removeFirst();
        const qint64 freed = static_cast<qint64>(m_cache.value(lruKey).sizeInBytes());
        m_cache.remove(lruKey);
        m_currentBytes -= freed;
    }
    if (m_currentBytes < 0) {
        m_currentBytes = 0;
    }
}

} // namespace playback
