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
