#include "Keyframe.h"
#include "EasingCurveModel.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kIdentityBezX1 = 0.0;
constexpr double kIdentityBezY1 = 0.0;
constexpr double kIdentityBezX2 = 1.0;
constexpr double kIdentityBezY2 = 1.0;

bool hasNonIdentityBezier(const KeyframePoint &kf)
{
    return kf.bezX1 != kIdentityBezX1
        || kf.bezY1 != kIdentityBezY1
        || kf.bezX2 != kIdentityBezX2
        || kf.bezY2 != kIdentityBezY2;
}

KeyframePoint::Interpolation interpolationFromJson(const QJsonObject &obj)
{
    if (obj[QStringLiteral("interp")].toString() == QStringLiteral("Bezier")) {
        return KeyframePoint::Bezier;
    }
    return static_cast<KeyframePoint::Interpolation>(
        obj[QStringLiteral("interpolation")].toInt(0));
}

QString loopModeToJsonValue(LoopMode mode)
{
    switch (mode) {
    case LoopMode::None:
        break;
    case LoopMode::Cycle:
        return QStringLiteral("Cycle");
    case LoopMode::PingPong:
        return QStringLiteral("PingPong");
    case LoopMode::Continue:
        return QStringLiteral("Continue");
    }
    return QString();
}

LoopMode loopModeFromJsonValue(const QJsonValue &value)
{
    if (value.isDouble()) {
        switch (value.toInt(static_cast<int>(LoopMode::None))) {
        case static_cast<int>(LoopMode::Cycle):
            return LoopMode::Cycle;
        case static_cast<int>(LoopMode::PingPong):
            return LoopMode::PingPong;
        case static_cast<int>(LoopMode::Continue):
            return LoopMode::Continue;
        default:
            return LoopMode::None;
        }
    }

    const QString mode = value.toString();
    if (mode.compare(QStringLiteral("Cycle"), Qt::CaseInsensitive) == 0)
        return LoopMode::Cycle;
    if (mode.compare(QStringLiteral("PingPong"), Qt::CaseInsensitive) == 0)
        return LoopMode::PingPong;
    if (mode.compare(QStringLiteral("Continue"), Qt::CaseInsensitive) == 0)
        return LoopMode::Continue;
    return LoopMode::None;
}

double positiveModulo(double value, double period)
{
    double phase = std::fmod(value, period);
    if (phase < 0.0)
        phase += period;
    return phase;
}

double loopedTrackValueAt(const KeyframeTrack &track, LoopMode mode, double time)
{
    if (mode == LoopMode::None)
        return track.valueAt(time);

    const QVector<KeyframePoint> &keyframes = track.keyframes();
    if (keyframes.size() < 2)
        return track.valueAt(time);

    const KeyframePoint &first = keyframes.first();
    const KeyframePoint &last = keyframes.last();
    if (time <= last.time)
        return track.valueAt(time);

    const double range = last.time - first.time;
    if (!std::isfinite(range) || range <= 0.0)
        return track.valueAt(time);

    switch (mode) {
    case LoopMode::None:
        return track.valueAt(time);
    case LoopMode::Cycle: {
        const double phase = positiveModulo(time - first.time, range);
        return track.valueAt(first.time + phase);
    }
    case LoopMode::PingPong: {
        const double phase = positiveModulo(time - first.time, 2.0 * range);
        const double foldedTime = phase > range
            ? last.time - (phase - range)
            : first.time + phase;
        return track.valueAt(foldedTime);
    }
    case LoopMode::Continue: {
        const KeyframePoint &prev = keyframes.at(keyframes.size() - 2);
        const double dt = last.time - prev.time;
        if (!std::isfinite(dt) || dt <= 0.0)
            return track.valueAt(time);
        const double slope = (last.value - prev.value) / dt;
        return last.value + slope * (time - last.time);
    }
    }

    return track.valueAt(time);
}

} // namespace

// --- KeyframeTrack ---

KeyframeTrack::KeyframeTrack(const QString &property, double defaultValue)
    : m_property(property), m_defaultValue(defaultValue) {}

void KeyframeTrack::addKeyframe(double time,
                                double value,
                                KeyframePoint::Interpolation interp,
                                double bezX1,
                                double bezY1,
                                double bezX2,
                                double bezY2,
                                bool hasSpatialTangent,
                                double spatialOutX,
                                double spatialOutY,
                                double spatialInX,
                                double spatialInY)
{
    // Remove existing keyframe at same time
    for (int i = 0; i < m_keyframes.size(); ++i) {
        if (std::abs(m_keyframes[i].time - time) < 0.001) {
            m_keyframes[i].value = value;
            m_keyframes[i].interpolation = interp;
            m_keyframes[i].bezX1 = bezX1;
            m_keyframes[i].bezY1 = bezY1;
            m_keyframes[i].bezX2 = bezX2;
            m_keyframes[i].bezY2 = bezY2;
            m_keyframes[i].hasSpatialTangent = hasSpatialTangent;
            m_keyframes[i].spatialOutX = spatialOutX;
            m_keyframes[i].spatialOutY = spatialOutY;
            m_keyframes[i].spatialInX = spatialInX;
            m_keyframes[i].spatialInY = spatialInY;
            return;
        }
    }

    KeyframePoint kf;
    kf.time = time;
    kf.value = value;
    kf.interpolation = interp;
    kf.bezX1 = bezX1;
    kf.bezY1 = bezY1;
    kf.bezX2 = bezX2;
    kf.bezY2 = bezY2;
    kf.hasSpatialTangent = hasSpatialTangent;
    kf.spatialOutX = spatialOutX;
    kf.spatialOutY = spatialOutY;
    kf.spatialInX = spatialInX;
    kf.spatialInY = spatialInY;
    m_keyframes.append(kf);

    // Keep sorted by time
    std::sort(m_keyframes.begin(), m_keyframes.end(),
        [](const KeyframePoint &a, const KeyframePoint &b) { return a.time < b.time; });
}

void KeyframeTrack::removeKeyframe(int index)
{
    if (index >= 0 && index < m_keyframes.size())
        m_keyframes.removeAt(index);
}

void KeyframeTrack::setKeyframeValue(int index, double value)
{
    if (index >= 0 && index < m_keyframes.size())
        m_keyframes[index].value = value;
}

void KeyframeTrack::setKeyframeTime(int index, double time)
{
    if (index >= 0 && index < m_keyframes.size()) {
        m_keyframes[index].time = time;
        std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const KeyframePoint &a, const KeyframePoint &b) { return a.time < b.time; });
    }
}

double KeyframeTrack::valueAt(double time) const
{
    if (m_keyframes.isEmpty()) return m_defaultValue;
    if (m_keyframes.size() == 1) return m_keyframes[0].value;

    // Before first keyframe
    if (time <= m_keyframes.first().time) return m_keyframes.first().value;
    // After last keyframe
    if (time >= m_keyframes.last().time) return m_keyframes.last().value;

    // Find surrounding keyframes
    for (int i = 0; i < m_keyframes.size() - 1; ++i) {
        const auto &a = m_keyframes[i];
        const auto &b = m_keyframes[i + 1];
        if (time >= a.time && time <= b.time) {
            double t = (time - a.time) / (b.time - a.time);
            return interpolate(a.value, b.value, t, a.interpolation,
                               a.bezX1, a.bezY1, a.bezX2, a.bezY2);
        }
    }
    return m_defaultValue;
}

double KeyframeTrack::interpolate(double from, double to, double t,
                                   KeyframePoint::Interpolation interp,
                                   double bezX1,
                                   double bezY1,
                                   double bezX2,
                                   double bezY2)
{
    switch (interp) {
    case KeyframePoint::Linear:
        return from + (to - from) * t;
    case KeyframePoint::EaseIn:
        return from + (to - from) * easeIn(t);
    case KeyframePoint::EaseOut:
        return from + (to - from) * easeOut(t);
    case KeyframePoint::EaseInOut:
        return from + (to - from) * easeInOut(t);
    case KeyframePoint::Hold:
        return from; // stay at 'from' until next keyframe
    case KeyframePoint::Bezier: {
        const easing::CubicBezier bez{bezX1, bezY1, bezX2, bezY2};
        const double easedT = easing::evaluate(easing::EasingType::CubicBezier, t, bez);
        return from + (to - from) * easedT;
    }
    case KeyframePoint::ElasticOut:
        return from + (to - from) * easing::elasticOut(t);
    case KeyframePoint::BounceOut:
        return from + (to - from) * easing::bounceOut(t);
    case KeyframePoint::BackOut:
        return from + (to - from) * easing::backOut(t);
    }
    return from + (to - from) * t;
}

double KeyframeTrack::easeIn(double t)     { return t * t; }
double KeyframeTrack::easeOut(double t)    { return t * (2.0 - t); }
double KeyframeTrack::easeInOut(double t)  { return t < 0.5 ? 2.0 * t * t : -1.0 + (4.0 - 2.0 * t) * t; }

void KeyframeTrack::addVariantKeyframe(double time, const QVariant &value,
                                        KeyframePoint::Interpolation interp)
{
    for (int i = 0; i < m_variantKeyframes.size(); ++i) {
        if (std::abs(m_variantKeyframes[i].time - time) < 0.001) {
            m_variantKeyframes[i].value = value.toDouble();
            m_variantKeyframes[i].interpolation = interp;
            return;
        }
    }

    KeyframePoint kf;
    kf.time = time;
    kf.value = value.toDouble();
    kf.interpolation = interp;
    m_variantKeyframes.append(kf);

    std::sort(m_variantKeyframes.begin(), m_variantKeyframes.end(),
        [](const KeyframePoint &a, const KeyframePoint &b) { return a.time < b.time; });
}

bool KeyframeTrack::isStringKeyframe() const
{
    return !m_variantKeyframes.isEmpty();
}

QString KeyframeTrack::stringValueAt(double time) const
{
    if (m_variantKeyframes.isEmpty()) return QString();

    // Snapping: find the LAST keyframe with time <= given time.
    // Note: this legacy path stores values as doubles; string-typed keyframes
    // now live on StringKeyframeTrack (see KeyframeManager::stringValueAt).
    for (int i = m_variantKeyframes.size() - 1; i >= 0; --i) {
        if (time >= m_variantKeyframes[i].time - 0.0001) {
            return QString::number(m_variantKeyframes[i].value);
        }
    }

    // Before first keyframe: return empty (no text before first keyframe)
    return QString();
}

QJsonObject keyframePointToJson(const KeyframePoint &kf)
{
    QJsonObject kfObj;
    kfObj[QStringLiteral("time")] = kf.time;
    kfObj[QStringLiteral("value")] = kf.value;
    kfObj[QStringLiteral("interpolation")] = static_cast<int>(kf.interpolation);
    if (kf.interpolation == KeyframePoint::Bezier) {
        kfObj[QStringLiteral("interp")] = QStringLiteral("Bezier");
        if (hasNonIdentityBezier(kf)) {
            kfObj[QStringLiteral("bezX1")] = kf.bezX1;
            kfObj[QStringLiteral("bezY1")] = kf.bezY1;
            kfObj[QStringLiteral("bezX2")] = kf.bezX2;
            kfObj[QStringLiteral("bezY2")] = kf.bezY2;
        }
    }
    if (kf.hasSpatialTangent) {
        kfObj[QStringLiteral("spatialOutX")] = kf.spatialOutX;
        kfObj[QStringLiteral("spatialOutY")] = kf.spatialOutY;
        kfObj[QStringLiteral("spatialInX")] = kf.spatialInX;
        kfObj[QStringLiteral("spatialInY")] = kf.spatialInY;
    }
    return kfObj;
}

KeyframePoint keyframePointFromJson(const QJsonObject &obj)
{
    KeyframePoint kf;
    kf.time = obj[QStringLiteral("time")].toDouble();
    kf.value = obj[QStringLiteral("value")].toDouble();
    kf.interpolation = interpolationFromJson(obj);
    kf.bezX1 = obj[QStringLiteral("bezX1")].toDouble(kIdentityBezX1);
    kf.bezY1 = obj[QStringLiteral("bezY1")].toDouble(kIdentityBezY1);
    kf.bezX2 = obj[QStringLiteral("bezX2")].toDouble(kIdentityBezX2);
    kf.bezY2 = obj[QStringLiteral("bezY2")].toDouble(kIdentityBezY2);
    kf.hasSpatialTangent = obj.contains(QStringLiteral("spatialOutX"))
        || obj.contains(QStringLiteral("spatialOutY"))
        || obj.contains(QStringLiteral("spatialInX"))
        || obj.contains(QStringLiteral("spatialInY"));
    kf.spatialOutX = obj[QStringLiteral("spatialOutX")].toDouble(0.0);
    kf.spatialOutY = obj[QStringLiteral("spatialOutY")].toDouble(0.0);
    kf.spatialInX = obj[QStringLiteral("spatialInX")].toDouble(0.0);
    kf.spatialInY = obj[QStringLiteral("spatialInY")].toDouble(0.0);
    return kf;
}

// --- StringKeyframeTrack ---

StringKeyframeTrack::StringKeyframeTrack(const QString &property, const QString &defaultValue)
    : m_property(property), m_defaultValue(defaultValue) {}

void StringKeyframeTrack::addKeyframe(double time, const QString &value)
{
    for (int i = 0; i < m_keyframes.size(); ++i) {
        if (std::abs(m_keyframes[i].time - time) < 0.001) {
            m_keyframes[i].value = value;
            return;
        }
    }

    StringKeyframePoint kf;
    kf.time = time;
    kf.value = value;
    m_keyframes.append(kf);

    std::sort(m_keyframes.begin(), m_keyframes.end(),
        [](const StringKeyframePoint &a, const StringKeyframePoint &b) { return a.time < b.time; });
}

void StringKeyframeTrack::removeKeyframe(int index)
{
    if (index >= 0 && index < m_keyframes.size())
        m_keyframes.removeAt(index);
}

void StringKeyframeTrack::setKeyframeValue(int index, const QString &value)
{
    if (index >= 0 && index < m_keyframes.size())
        m_keyframes[index].value = value;
}

void StringKeyframeTrack::setKeyframeTime(int index, double time)
{
    if (index >= 0 && index < m_keyframes.size()) {
        m_keyframes[index].time = time;
        std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const StringKeyframePoint &a, const StringKeyframePoint &b) { return a.time < b.time; });
    }
}

QString StringKeyframeTrack::valueAt(double time) const
{
    if (m_keyframes.isEmpty()) return m_defaultValue;

    // Snapping: LAST keyframe with time <= given time
    for (int i = m_keyframes.size() - 1; i >= 0; --i) {
        if (time >= m_keyframes[i].time - 0.0001) {
            return m_keyframes[i].value;
        }
    }

    // Before first keyframe
    return m_defaultValue;
}

// --- KeyframeManager ---

void KeyframeManager::addTrack(const KeyframeTrack &track)
{
    // Replace if exists
    for (auto &t : m_tracks) {
        if (t.propertyName() == track.propertyName()) {
            t = track;
            return;
        }
    }
    m_tracks.append(track);
}

void KeyframeManager::removeTrack(const QString &propertyName)
{
    m_tracks.erase(
        std::remove_if(m_tracks.begin(), m_tracks.end(),
            [&](const KeyframeTrack &t) { return t.propertyName() == propertyName; }),
        m_tracks.end());
    m_loopOutModes.remove(propertyName);
}

KeyframeTrack *KeyframeManager::track(const QString &propertyName)
{
    for (auto &t : m_tracks)
        if (t.propertyName() == propertyName) return &t;
    return nullptr;
}

const KeyframeTrack *KeyframeManager::track(const QString &propertyName) const
{
    for (const auto &t : m_tracks)
        if (t.propertyName() == propertyName) return &t;
    return nullptr;
}

bool KeyframeManager::hasTrack(const QString &propertyName) const
{
    return track(propertyName) != nullptr;
}

bool KeyframeManager::hasAnyKeyframes() const
{
    for (const auto &t : m_tracks)
        if (t.hasKeyframes()) return true;
    return false;
}

double KeyframeManager::valueAt(const QString &propertyName, double time, double defaultVal) const
{
    const auto *t = track(propertyName);
    if (!t)
        return defaultVal;

    const LoopMode mode = loopOutMode(propertyName);
    if (mode == LoopMode::None)
        return t->valueAt(time);
    return loopedTrackValueAt(*t, mode, time);
}

QString KeyframeManager::stringValueAt(const QString &propertyName, double time, const QString &defaultVal) const
{
    // US-AETEXT-4 FIX: route through StringKeyframeTrack (not KeyframeTrack::stringValueAt
    // which goes through addVariantKeyframe -> value.toDouble() and destroys string content)
    const auto *st = stringTrack(propertyName);
    return st ? st->valueAt(time) : defaultVal;
}

// --- US-AETEXT-4: String track management ---

void KeyframeManager::addStringTrack(const StringKeyframeTrack &track)
{
    for (auto &t : m_stringTracks) {
        if (t.propertyName() == track.propertyName()) {
            t = track;
            return;
        }
    }
    m_stringTracks.append(track);
}

void KeyframeManager::removeStringTrack(const QString &propertyName)
{
    m_stringTracks.erase(
        std::remove_if(m_stringTracks.begin(), m_stringTracks.end(),
            [&](const StringKeyframeTrack &t) { return t.propertyName() == propertyName; }),
        m_stringTracks.end());
}

StringKeyframeTrack *KeyframeManager::stringTrack(const QString &propertyName)
{
    for (auto &t : m_stringTracks)
        if (t.propertyName() == propertyName) return &t;
    return nullptr;
}

const StringKeyframeTrack *KeyframeManager::stringTrack(const QString &propertyName) const
{
    for (const auto &t : m_stringTracks)
        if (t.propertyName() == propertyName) return &t;
    return nullptr;
}

bool KeyframeManager::hasStringTrack(const QString &propertyName) const
{
    return stringTrack(propertyName) != nullptr;
}

void KeyframeManager::setLoopOutMode(const QString &propertyName, LoopMode mode)
{
    if (mode == LoopMode::None) {
        m_loopOutModes.remove(propertyName);
        return;
    }
    m_loopOutModes.insert(propertyName, mode);
}

LoopMode KeyframeManager::loopOutMode(const QString &propertyName) const
{
    return m_loopOutModes.value(propertyName, LoopMode::None);
}

// --- US-AETEXT-4: KeyframeManager serialisation ---

QJsonObject KeyframeManager::toJson() const
{
    QJsonObject obj;

    // Numeric tracks
    QJsonArray tracksArray;
    for (const auto &track : m_tracks) {
        QJsonObject trackObj;
        trackObj[QStringLiteral("property")] = track.propertyName();
        trackObj[QStringLiteral("defaultValue")] = track.defaultValue();

        QJsonArray keyframesArray;
        for (const auto &kf : track.keyframes()) {
            keyframesArray.append(keyframePointToJson(kf));
        }
        trackObj[QStringLiteral("keyframes")] = keyframesArray;
        const LoopMode loopOut = loopOutMode(track.propertyName());
        if (loopOut != LoopMode::None) {
            trackObj[QStringLiteral("loopOut")] = loopModeToJsonValue(loopOut);
        }
        tracksArray.append(trackObj);
    }
    obj[QStringLiteral("tracks")] = tracksArray;

    // String tracks (US-AETEXT-4)
    QJsonArray stringTracksArray;
    for (const auto &track : m_stringTracks) {
        QJsonObject trackObj;
        trackObj[QStringLiteral("property")] = track.propertyName();
        trackObj[QStringLiteral("defaultValue")] = track.defaultValue();

        QJsonArray keyframesArray;
        for (const auto &kf : track.keyframes()) {
            QJsonObject kfObj;
            kfObj[QStringLiteral("time")] = kf.time;
            kfObj[QStringLiteral("value")] = kf.value;
            keyframesArray.append(kfObj);
        }
        trackObj[QStringLiteral("keyframes")] = keyframesArray;
        stringTracksArray.append(trackObj);
    }
    obj[QStringLiteral("stringTracks")] = stringTracksArray;

    return obj;
}

void KeyframeManager::fromJson(const QJsonObject &obj)
{
    m_tracks.clear();
    m_stringTracks.clear();
    m_loopOutModes.clear();

    // Numeric tracks
    QJsonArray tracksArray = obj[QStringLiteral("tracks")].toArray();
    for (const auto &trackVal : tracksArray) {
        QJsonObject trackObj = trackVal.toObject();
        QString property = trackObj[QStringLiteral("property")].toString();
        double defaultValue = trackObj[QStringLiteral("defaultValue")].toDouble(0.0);

        KeyframeTrack track(property, defaultValue);
        QJsonArray keyframesArray = trackObj[QStringLiteral("keyframes")].toArray();
        for (const auto &kfVal : keyframesArray) {
            const KeyframePoint kf = keyframePointFromJson(kfVal.toObject());
            track.addKeyframe(kf.time, kf.value, kf.interpolation,
                              kf.bezX1, kf.bezY1, kf.bezX2, kf.bezY2,
                              kf.hasSpatialTangent, kf.spatialOutX,
                              kf.spatialOutY, kf.spatialInX, kf.spatialInY);
        }
        const LoopMode loopOut =
            loopModeFromJsonValue(trackObj.value(QStringLiteral("loopOut")));
        if (loopOut != LoopMode::None)
            m_loopOutModes.insert(property, loopOut);
        m_tracks.append(track);
    }

    // String tracks (US-AETEXT-4)
    QJsonArray stringTracksArray = obj[QStringLiteral("stringTracks")].toArray();
    for (const auto &trackVal : stringTracksArray) {
        QJsonObject trackObj = trackVal.toObject();
        QString property = trackObj[QStringLiteral("property")].toString();
        QString defaultValue = trackObj[QStringLiteral("defaultValue")].toString();

        StringKeyframeTrack track(property, defaultValue);
        QJsonArray keyframesArray = trackObj[QStringLiteral("keyframes")].toArray();
        for (const auto &kfVal : keyframesArray) {
            QJsonObject kfObj = kfVal.toObject();
            double time = kfObj[QStringLiteral("time")].toDouble();
            QString value = kfObj[QStringLiteral("value")].toString();
            track.addKeyframe(time, value);
        }
        m_stringTracks.append(track);
    }
}

// US-BRUSH-5: idempotent helper — adds a 'brush_progress' KeyframeTrack
// with default 0.0 if not already present.
void ensureBrushProgressTrack(KeyframeManager &manager)
{
    if (!manager.hasTrack(QStringLiteral("brush_progress"))) {
        manager.addTrack(KeyframeTrack(QStringLiteral("brush_progress"), 0.0));
    }
}
