#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QFont>
#include <QColor>
#include <QJsonObject>
#include <QPainter>
#include <QRectF>
#include "SubtitleGenerator.h"
#include "TextManager.h"

// Style configuration for subtitle track rendering
struct SubtitleStyle {
    QFont font = QFont("Arial", 28, QFont::Bold);
    QColor color = Qt::white;
    QColor outlineColor = Qt::black;
    double outlineWidth = 2.0;
    bool boxEnabled = false;
    QColor boxColor = Qt::black;
    double boxOpacity = 0.6;
    double verticalPos = 0.85;       // 0..1 from top
    double maxWidthFraction = 0.8;   // fraction of canvas width
    int alignment = Qt::AlignHCenter;
    bool karaokeEnabled = false;
    QColor karaokeHighlightColor = QColor(255, 210, 0);
};

class SubtitleTrackRenderer : public QObject
{
    Q_OBJECT

public:
    explicit SubtitleTrackRenderer(QObject *parent = nullptr);

    void setSegments(const QVector<SubtitleSegment> &segments);
    const QVector<SubtitleSegment> &segments() const { return m_segments; }

    void setStyle(const SubtitleStyle &style);
    SubtitleStyle style() const { return m_style; }

    // Returns the text of the segment whose [startTime, endTime) contains timeSec;
    // returns empty QString when no segment is active.
    QString textAt(double timeSec) const;

    // Draws the active segment's wrapped text with outline + optional background box.
    void paintOnto(QPainter &painter, const QRectF &canvasRect, double timeSec) const;

    // Convert to the editor's overlay model so existing GLPreview rendering picks it up.
    QVector<EnhancedTextOverlay> toOverlays() const;

    // Serialization
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &json);

private:
    // Word-wrap text to fit within maxWidth pixels, returning list of lines.
    static QStringList wrapText(const QString &text, const QFont &font, int maxWidth);
    const SubtitleSegment* activeSegmentAt(double timeSec) const;

    QVector<SubtitleSegment> m_segments;
    SubtitleStyle m_style;
};
