#pragma once

#include <QVariant>
#include <QString>
#include <QVector>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>

enum class LoopMode {
    None = 0,
    Cycle,
    PingPong,
    Continue
};

struct KeyframePoint {
    double time = 0.0;   // seconds
    double value = 0.0;

    enum Interpolation { Linear, EaseIn, EaseOut, EaseInOut, Hold, Bezier, ElasticOut, BounceOut, BackOut };
    Interpolation interpolation = Linear;
    double bezX1 = 0.0;
    double bezY1 = 0.0;
    double bezX2 = 1.0;
    double bezY2 = 1.0;
    bool hasSpatialTangent = false;
    double spatialOutX = 0.0;
    double spatialOutY = 0.0;
    double spatialInX = 0.0;
    double spatialInY = 0.0;
};

struct StringKeyframePoint {
    double time = 0.0;
    QString value;
};

class KeyframeTrack
{
public:
    KeyframeTrack() = default;
    explicit KeyframeTrack(const QString &property, double defaultValue = 0.0);

    const QString &propertyName() const { return m_property; }
    double defaultValue() const { return m_defaultValue; }

    void addKeyframe(double time, double value,
                     KeyframePoint::Interpolation interp = KeyframePoint::Linear,
                     double bezX1 = 0.0,
                     double bezY1 = 0.0,
                     double bezX2 = 1.0,
                     double bezY2 = 1.0,
                     bool hasSpatialTangent = false,
                     double spatialOutX = 0.0,
                     double spatialOutY = 0.0,
                     double spatialInX = 0.0,
                     double spatialInY = 0.0);
    void removeKeyframe(int index);
    void setKeyframeValue(int index, double value);
    void setKeyframeTime(int index, double time);

    double valueAt(double time) const;
    bool hasKeyframes() const { return !m_keyframes.isEmpty(); }
    int count() const { return m_keyframes.size(); }
    const QVector<KeyframePoint> &keyframes() const { return m_keyframes; }

    // US-AETEXT-4: QVariant payload helpers
    void addVariantKeyframe(double time, const QVariant &value,
                            KeyframePoint::Interpolation interp = KeyframePoint::Hold);
    bool isStringKeyframe() const;
    QString stringValueAt(double time) const;

private:
    static double interpolate(double from, double to, double t,
                              KeyframePoint::Interpolation interp,
                              double bezX1 = 0.0,
                              double bezY1 = 0.0,
                              double bezX2 = 1.0,
                              double bezY2 = 1.0);
    static double easeIn(double t);
    static double easeOut(double t);
    static double easeInOut(double t);

    QString m_property;
    double m_defaultValue = 0.0;
    QVector<KeyframePoint> m_keyframes; // sorted by time
    QVector<KeyframePoint> m_variantKeyframes; // sorted by time, uses interpolation=Hold for discrete
};

QJsonObject keyframePointToJson(const KeyframePoint &kf);
KeyframePoint keyframePointFromJson(const QJsonObject &obj);

// US-AETEXT-4: String keyframe track for Source Text animation
class StringKeyframeTrack
{
public:
    StringKeyframeTrack() = default;
    explicit StringKeyframeTrack(const QString &property, const QString &defaultValue = QString());

    const QString &propertyName() const { return m_property; }
    const QString &defaultValue() const { return m_defaultValue; }

    void addKeyframe(double time, const QString &value);
    void removeKeyframe(int index);
    void setKeyframeValue(int index, const QString &value);
    void setKeyframeTime(int index, double time);

    // Snapping: returns the LAST keyframe value with time <= given time
    QString valueAt(double time) const;
    bool hasKeyframes() const { return !m_keyframes.isEmpty(); }
    int count() const { return m_keyframes.size(); }
    const QVector<StringKeyframePoint> &keyframes() const { return m_keyframes; }

private:
    QString m_property;
    QString m_defaultValue;
    QVector<StringKeyframePoint> m_keyframes; // sorted by time
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

    // AE-ANIM-3: engine/JSON support only. The editor combo box is deferred
    // so the default None path remains byte-identical for existing projects.
    void setLoopOutMode(const QString &propertyName, LoopMode mode);
    LoopMode loopOutMode(const QString &propertyName) const;

    // US-AETEXT-4: String track management (routes source-text keyframes through StringKeyframeTrack)
    void addStringTrack(const StringKeyframeTrack &track);
    void removeStringTrack(const QString &propertyName);
    StringKeyframeTrack *stringTrack(const QString &propertyName);
    const StringKeyframeTrack *stringTrack(const QString &propertyName) const;
    bool hasStringTrack(const QString &propertyName) const;

    QVector<StringKeyframeTrack> &stringTracks() { return m_stringTracks; }
    const QVector<StringKeyframeTrack> &stringTracks() const { return m_stringTracks; }

    // Convenience: get value at time for a named property, returns defaultVal if no track
    double valueAt(const QString &propertyName, double time, double defaultVal = 0.0) const;

    // US-AETEXT-4: get string value at time for a named property (uses StringKeyframeTrack)
    QString stringValueAt(const QString &propertyName, double time, const QString &defaultVal = QString()) const;

    // US-AETEXT-4: Serialisation
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    QVector<KeyframeTrack> m_tracks;
    QVector<StringKeyframeTrack> m_stringTracks; // US-AETEXT-4: dedicated string-typed tracks
    QHash<QString, LoopMode> m_loopOutModes;
};

// US-BRUSH-5: idempotent helper — adds a 'brush_progress' KeyframeTrack
// with default 0.0 if not already present on the given KeyframeManager.
void ensureBrushProgressTrack(KeyframeManager &manager);
