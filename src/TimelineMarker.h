#pragma once

#include <QString>
#include <QVector>
#include <QColor>
#include <QJsonObject>
#include <QJsonArray>

// --- Marker enums ---

enum class MarkerType { Standard, Chapter, Todo, Note };

enum class MarkerColor { Red, Orange, Yellow, Green, Blue, Purple, White };

inline QColor markerColorToQColor(MarkerColor c)
{
    switch (c) {
    case MarkerColor::Red:    return QColor(255, 60, 60);
    case MarkerColor::Orange: return QColor(255, 165, 0);
    case MarkerColor::Yellow: return QColor(255, 220, 40);
    case MarkerColor::Green:  return QColor(60, 200, 60);
    case MarkerColor::Blue:   return QColor(60, 120, 255);
    case MarkerColor::Purple: return QColor(180, 80, 220);
    case MarkerColor::White:  return QColor(255, 255, 255);
    }
    return QColor(255, 255, 255);
}

// --- TimelineMarker ---

struct TimelineMarker {
    double time = 0.0;       // seconds
    QString name;
    MarkerType type = MarkerType::Standard;
    MarkerColor color = MarkerColor::Blue;
    QString comment;

    QJsonObject toJson() const;
    static TimelineMarker fromJson(const QJsonObject &obj);
};

// --- Chapter ---

struct Chapter {
    double startTime = 0.0;  // seconds
    double endTime = 0.0;
    QString title;
    int index = 0;
};

// --- MarkerManager ---

class MarkerManager
{
public:
    MarkerManager() = default;

    // Mutate
    void addMarker(double time, const QString &name,
                   MarkerType type = MarkerType::Standard,
                   MarkerColor color = MarkerColor::Blue);
    void removeMarker(int index);
    void clearMarkers();

    // Query
    const QVector<TimelineMarker> &markers() const { return m_markers; }
    int count() const { return m_markers.size(); }
    bool isEmpty() const { return m_markers.isEmpty(); }

    // Search / navigation
    int markerAt(double time, double tolerance = 0.1) const;
    QVector<TimelineMarker> markersByType(MarkerType type) const;
    int nextMarker(double fromTime) const;
    int prevMarker(double fromTime) const;

    // Chapter generation
    QVector<Chapter> generateChapters() const;
    QString exportYouTubeChapters() const;
    QString exportChapterMetadata() const;

    // Import
    int importFromSRT(const QString &filePath);

    // Serialisation
    QJsonArray toJson() const;
    void fromJson(const QJsonArray &arr);

private:
    void sortMarkers();

    QVector<TimelineMarker> m_markers; // sorted by time
};
