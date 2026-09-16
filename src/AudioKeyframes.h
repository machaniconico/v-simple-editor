#pragma once

#include "Keyframe.h"
#include <QHash>
#include <QStringList>
#include <memory>
#include <mutex>

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
class EnvelopeCache {
public:
    std::shared_ptr<const Envelope> envelope(const QString& filePath);
private:
    struct Entry {
        std::shared_ptr<const Envelope> value;
        qint64 size = -1;
        qint64 modified = -1;
    };
    std::mutex m_mutex;
    QHash<QString, Entry> m_entries;
    QStringList m_lru;
};

// Acceptance instrumentation: bypass/cache observation for unused-feature checks.
void setDisabledForTest(bool disabled);
void resetInvocationCountForTest();
int invocationCountForTest();
} // namespace audiokf
