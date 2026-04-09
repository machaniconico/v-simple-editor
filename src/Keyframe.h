#pragma once

#include <QString>
#include <QVector>

struct KeyframePoint {
    double time = 0.0;   // seconds
    double value = 0.0;

    enum Interpolation { Linear, EaseIn, EaseOut, EaseInOut, Hold };
    Interpolation interpolation = Linear;
};

class KeyframeTrack
{
public:
    KeyframeTrack() = default;
    explicit KeyframeTrack(const QString &property, double defaultValue = 0.0);

    const QString &propertyName() const { return m_property; }
    double defaultValue() const { return m_defaultValue; }

    void addKeyframe(double time, double value,
                     KeyframePoint::Interpolation interp = KeyframePoint::Linear);
    void removeKeyframe(int index);
    void setKeyframeValue(int index, double value);
    void setKeyframeTime(int index, double time);

    double valueAt(double time) const;
    bool hasKeyframes() const { return !m_keyframes.isEmpty(); }
    int count() const { return m_keyframes.size(); }
    const QVector<KeyframePoint> &keyframes() const { return m_keyframes; }

private:
    static double interpolate(double from, double to, double t,
                              KeyframePoint::Interpolation interp);
    static double easeIn(double t);
    static double easeOut(double t);
    static double easeInOut(double t);

    QString m_property;
    double m_defaultValue = 0.0;
    QVector<KeyframePoint> m_keyframes; // sorted by time
};

// Per-clip keyframe collection
class KeyframeManager
{
public:
    void addTrack(const KeyframeTrack &track);
    void removeTrack(const QString &propertyName);
    KeyframeTrack *track(const QString &propertyName);
    const KeyframeTrack *track(const QString &propertyName) const;
    bool hasTrack(const QString &propertyName) const;
    bool hasAnyKeyframes() const;

    QVector<KeyframeTrack> &tracks() { return m_tracks; }
    const QVector<KeyframeTrack> &tracks() const { return m_tracks; }

    // Convenience: get value at time for a named property, returns defaultVal if no track
    double valueAt(const QString &propertyName, double time, double defaultVal = 0.0) const;

private:
    QVector<KeyframeTrack> m_tracks;
};
