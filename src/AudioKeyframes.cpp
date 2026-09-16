#include "AudioKeyframes.h"
#include "Timeline.h"
#include "libavcore/AudioExtract.h"
#include <QDateTime>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace audiokf {
namespace {
std::atomic<bool> disabled{false};
std::atomic<int> invocations{0};
}
void setDisabledForTest(bool value) { disabled = value; }
void resetInvocationCountForTest() { invocations = 0; }
int invocationCountForTest() { return invocations.load(); }

Envelope computeEnvelope(const QVector<float>& mono, int sampleRate,
                         double fps, double smoothingMs)
{
    Envelope result;
    if (sampleRate <= 0 || !std::isfinite(fps) || fps <= 0.0
        || fps > sampleRate || !std::isfinite(smoothingMs) || smoothingMs < 0.0)
        return result;
    result.fps = fps;
    const double count = std::ceil(double(mono.size()) * fps / sampleRate);
    if (count > std::numeric_limits<int>::max()) return result;
    result.rms.reserve(int(count));
    const double alpha = smoothingMs > 0.0
        ? -std::expm1(-1000.0 / (fps * smoothingMs)) : 1.0;
    double previous = 0.0;
    for (int frame = 0; frame < int(count); ++frame) {
        const qsizetype begin = qsizetype(std::floor(double(frame) * sampleRate / fps));
        const qsizetype end = std::min(mono.size(),
            qsizetype(std::floor(double(frame + 1) * sampleRate / fps)));
        double sum = 0.0;
        for (qsizetype i = begin; i < end; ++i) {
            const double v = std::isfinite(mono[i]) ? double(mono[i]) : 0.0;
            sum += v * v;
        }
        const double rms = end > begin ? std::sqrt(sum / double(end - begin)) : 0.0;
        previous += alpha * (std::clamp(rms, 0.0, 1.0) - previous);
        result.rms.append(float(previous));
    }
    return result;
}

QVector<KeyframePoint> toKeyframes(const Envelope& envelope, double minValue,
                                  double maxValue, double threshold01, double offset)
{
    QVector<KeyframePoint> result;
    if (!std::isfinite(envelope.fps) || envelope.fps <= 0.0
        || !std::isfinite(envelope.startSec) || !std::isfinite(offset)
        || !std::isfinite(minValue) || !std::isfinite(maxValue)
        || !std::isfinite(threshold01)) return result;
    result.reserve(envelope.rms.size());
    for (qsizetype i = 0; i < envelope.rms.size(); ++i) {
        double v = std::isfinite(envelope.rms[i])
            ? std::clamp(double(envelope.rms[i]), 0.0, 1.0) : 0.0;
        if (v < std::clamp(threshold01, 0.0, 1.0)) v = 0.0;
        KeyframePoint point;
        point.time = envelope.startSec + offset + double(i) / envelope.fps;
        point.value = minValue + v * (maxValue - minValue);
        point.interpolation = KeyframePoint::Linear;
        result.append(point);
    }
    return result;
}

KeyframeTrack propertyTrack(const QString& path, const QVector<KeyframePoint>& points,
                            double defaultValue)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("transform.opacity"), QStringLiteral("motion.opacity")},
        {QStringLiteral("transform.scale"), QStringLiteral("motion.scale")},
        {QStringLiteral("transform.rotation"), QStringLiteral("motion.rotation")},
        {QStringLiteral("transform.position.x"), QStringLiteral("motion.position.x")},
        {QStringLiteral("transform.position.y"), QStringLiteral("motion.position.y")}
    };
    const auto it = names.constFind(path);
    if (it == names.cend()) return {};
    const double factor = path == QStringLiteral("transform.opacity") ? 0.01 : 1.0;
    KeyframeTrack track(it.value(), defaultValue * factor);
    for (const auto& point : points)
        track.addKeyframe(point.time, point.value * factor, point.interpolation);
    return track;
}

double levelAt(const Envelope& envelope, double seconds)
{
    if (!std::isfinite(seconds) || !std::isfinite(envelope.fps)
        || envelope.fps <= 0.0 || !std::isfinite(envelope.startSec)) return 0.0;
    const double index = (seconds - envelope.startSec) * envelope.fps;
    if (index < 0.0 || index >= double(envelope.rms.size())) return 0.0;
    return std::clamp(double(envelope.rms[qsizetype(index)]), 0.0, 1.0);
}

bool decodeMono(const QString& filePath, QVector<float>& mono,
                int& sampleRate, QString* error)
{
    mono.clear();
    sampleRate = 0;
    QTemporaryDir temp;
    if (!temp.isValid()) {
        if (error) *error = QStringLiteral("音声の一時フォルダーを作成できません。");
        return false;
    }
    const QString wav = temp.filePath(QStringLiteral("audio.wav"));
    std::vector<double> samples;
    if (!libavcore::extractAudioToWav(filePath, wav, 48000, error)
        || !libavcore::readPcm16WavToMono(wav, samples, sampleRate, error)) return false;
    mono.reserve(qsizetype(samples.size()));
    for (double sample : samples) mono.append(float(sample));
    return !mono.isEmpty();
}

std::shared_ptr<const Envelope> EnvelopeCache::envelope(const QString& filePath)
{
    if (disabled.load()) return std::make_shared<const Envelope>();
    ++invocations;
    const QFileInfo info(filePath);
    const QString key = info.absoluteFilePath();
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.constFind(key);
    if (it != m_entries.cend() && it->size == info.size() && it->modified == modified) {
        m_lru.removeAll(key);
        m_lru.append(key);
        return it->value;
    }
    QVector<float> mono;
    int sr = 0;
    auto value = std::make_shared<Envelope>();
    if (decodeMono(filePath, mono, sr)) *value = computeEnvelope(mono, sr, 60.0, 50.0);
    m_lru.removeAll(key);
    m_entries.remove(key);
    while (m_entries.size() >= 16) m_entries.remove(m_lru.takeFirst());
    m_entries.insert(key, Entry{value, info.size(), modified});
    m_lru.append(key);
    return value;
}

std::function<double(double)> makeLinkedAudioSampler(
    int linkGroup, double clipStart,
    const QVector<const QVector<ClipInfo>*>& audioTracks,
    const std::shared_ptr<EnvelopeCache>& cache)
{
    if (linkGroup <= 0 || !cache) return {};
    for (const auto* clips : audioTracks) {
        if (!clips) continue;
        double cursor = 0.0;
        for (const ClipInfo& audio : *clips) {
            const double start = cursor + qMax(0.0, audio.leadInSec);
            cursor = start + audio.effectiveDuration();
            if (audio.linkGroup != linkGroup || audio.filePath.isEmpty()) continue;
            // Only audioLevel() invokes this closure: ordinary expressions never decode.
            return [cache, audio, start, clipStart](double local) {
                const double audioLocal = clipStart + local - start;
                if (!std::isfinite(audioLocal) || audioLocal < 0.0
                    || audioLocal >= audio.effectiveDuration()) return 0.0;
                const auto envelope = cache->envelope(audio.filePath);
                return levelAt(*envelope, audio.sourceSecondAtLocalTime(audioLocal));
            };
        }
    }
    return {};
}
} // namespace audiokf
