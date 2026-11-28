#pragma once

#include "Keyframe.h"
#include <QHash>
#include <QSet>
#include <QStringList>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

struct ClipInfo;
class QThreadPool;

namespace audiokf {
struct Envelope {
    QVector<float> rms;
    double fps = 0.0;
    double startSec = 0.0;
};
Envelope computeEnvelope(const QVector<float>& mono, int sampleRate,
                         double fps, double smoothingMs);
QVector<KeyframePoint> toKeyframes(const Envelope& envelope, double minValue,
                                  double maxValue, double threshold01,
                                  double clipLocalOffsetSec);
double levelAt(const Envelope& envelope, double seconds);
// Expression property paths map to the existing motion tracks consumed by
// clipanim in both preview and export. Opacity is expressed as percent in UI.
KeyframeTrack propertyTrack(const QString& propertyPath,
                            const QVector<KeyframePoint>& points, double defaultValue);

// In-process decode, also used by the conversion dialog. No GUI dependency.
bool decodeMono(const QString& filePath, QVector<float>& mono,
                int& sampleRate, QString* error = nullptr);

// Thread-safe LRU. Returned snapshots stay alive across eviction and export.
// Failed decodes are cached as silence until the source file changes.
// US-1505: decodeMono runs with m_mutex released, so a cached file is served while
// another file decodes; a second caller of a file being decoded waits for it once
// (m_inFlight / m_decoded) and then reads the cache again.
class EnvelopeCache {
public:
    std::shared_ptr<const Envelope> envelope(const QString& filePath);
    // US-1710: decode filePaths on prefetchPool() so the first audioLevel() read finds
    // them cached. A worker decodes only while self still lives (weak_ptr::lock), so a
    // cache released meanwhile is not kept alive past the decode in flight.
    static void prefetch(std::weak_ptr<EnvelopeCache> self, QStringList filePaths);
    // Acceptance seam: true when envelope(filePath) would be served from the cache
    // (same key and size / mtime match as envelope()).
    bool isCachedForTest(const QString& filePath);
private:
    struct Entry {
        std::shared_ptr<const Envelope> value;
        qint64 size = -1;
        qint64 modified = -1;
    };
    std::mutex m_mutex;
    std::condition_variable m_decoded;
    QSet<QString> m_inFlight;
    QHash<QString, Entry> m_entries;
    QStringList m_lru;
};

// US-1710: the pool EnvelopeCache::prefetch runs on (2 threads, outside any cache).
QThreadPool& prefetchPool();

// US-1709: the one EnvelopeCache of the process, shared by the live Timeline's
// snapshot (MainWindow) and the Timelines that file-based render jobs load.
std::shared_ptr<EnvelopeCache> sharedEnvelopeCache();

// Clip lists are inspected only during construction; the sampler owns its source
// snapshot and cache. No timeline widgets are needed, and decoding stays lazy.
std::function<double(double)> makeLinkedAudioSampler(
    int linkGroup, double clipStart,
    const QVector<const QVector<ClipInfo>*>& audioTracks,
    const std::shared_ptr<EnvelopeCache>& cache);

// US-1505: what an expression's audioLevel() reads, copied by value on the UI thread
// (Timeline::expressionAudioEnvForRead): the audio tracks' clips, the shared envelope
// cache (decoding stays lazy) and the Timeline clip generation the copy was taken at.
// Immutable once published; render threads only read it.
struct ExprAudioEnv {
    QVector<QVector<ClipInfo>> audioClipsByTrack;
    std::shared_ptr<EnvelopeCache> cache;
    quint64 generation = 0;
};

// makeLinkedAudioSampler over the copied tracks of env (the sampler keeps its own
// copy of the linked clip and the cache, not env).
std::function<double(double)> makeLinkedAudioSamplerFrom(
    const ExprAudioEnv& env, int linkGroup, double clipStart);

// Acceptance instrumentation: bypass/cache observation for unused-feature checks.
void setDisabledForTest(bool disabled);
void resetInvocationCountForTest();
int invocationCountForTest();
// US-1505: sleep ms right before each EnvelopeCache decode (0 = none, the default);
// decodeDelayEntriesForTest counts the decodes that entered that sleep.
void setDecodeDelayForTest(int ms);
int decodeDelayEntriesForTest();
} // namespace audiokf
