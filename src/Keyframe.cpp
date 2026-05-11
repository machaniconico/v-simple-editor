#include "Keyframe.h"
#include <algorithm>
#include <cmath>

// --- KeyframeTrack ---

KeyframeTrack::KeyframeTrack(const QString &property, double defaultValue)
    : m_property(property), m_defaultValue(defaultValue) {}

void KeyframeTrack::addKeyframe(double time, double value, KeyframePoint::Interpolation interp)
{
    // Remove existing keyframe at same time
    for (int i = 0; i < m_keyframes.size(); ++i) {
        if (std::abs(m_keyframes[i].time - time) < 0.001) {
            m_keyframes[i].value = value;
            m_keyframes[i].interpolation = interp;
            return;
        }
    }

    KeyframePoint kf;
    kf.time = time;
    kf.value = value;
    kf.interpolation = interp;
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
            return interpolate(a.value, b.value, t, a.interpolation);
        }
    }
    return m_defaultValue;
}

double KeyframeTrack::interpolate(double from, double to, double t,
                                   KeyframePoint::Interpolation interp)
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
    return t ? t->valueAt(time) : defaultVal;
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
            QJsonObject kfObj;
            kfObj[QStringLiteral("time")] = kf.time;
            kfObj[QStringLiteral("value")] = kf.value;
            kfObj[QStringLiteral("interpolation")] = static_cast<int>(kf.interpolation);
            keyframesArray.append(kfObj);
        }
        trackObj[QStringLiteral("keyframes")] = keyframesArray;
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

    // Numeric tracks
    QJsonArray tracksArray = obj[QStringLiteral("tracks")].toArray();
    for (const auto &trackVal : tracksArray) {
        QJsonObject trackObj = trackVal.toObject();
        QString property = trackObj[QStringLiteral("property")].toString();
        double defaultValue = trackObj[QStringLiteral("defaultValue")].toDouble(0.0);

        KeyframeTrack track(property, defaultValue);
        QJsonArray keyframesArray = trackObj[QStringLiteral("keyframes")].toArray();
        for (const auto &kfVal : keyframesArray) {
            QJsonObject kfObj = kfVal.toObject();
            double time = kfObj[QStringLiteral("time")].toDouble();
            double value = kfObj[QStringLiteral("value")].toDouble();
            auto interp = static_cast<KeyframePoint::Interpolation>(kfObj[QStringLiteral("interpolation")].toInt(0));
            track.addKeyframe(time, value, interp);
        }
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
