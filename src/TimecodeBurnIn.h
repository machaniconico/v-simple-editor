#pragma once

#include <QJsonObject>
#include <QRectF>
#include <QString>

class QPainter;
class Timeline;

struct TimecodeBurnInSettings {
    enum Position {
        TopLeft,
        TopCenter,
        TopRight,
        BottomLeft,
        BottomCenter,
        BottomRight
    };

    bool enabled = false;
    Position position = BottomCenter;
    int fontSizePct = 4;
    bool showFrames = true;
    bool dropFrame = false;
    QString prefix;
    bool showClipName = false;
    bool showDate = false;
    double opacity = 0.8;

    QJsonObject toJson() const;
    static TimecodeBurnInSettings fromJson(const QJsonObject &object);

    static QString positionName(Position position);
    static bool positionFromName(const QString &name, Position *position);
};

class TimecodeBurnInRenderer
{
public:
    explicit TimecodeBurnInRenderer(
        const TimecodeBurnInSettings &settings = TimecodeBurnInSettings{});

    void setSettings(const TimecodeBurnInSettings &settings);
    const TimecodeBurnInSettings &settings() const { return m_settings; }

    QString displayText(double timelineSec, double frameRate,
                        const QString &clipName = QString()) const;
    void paintOnto(QPainter &painter, const QRectF &frameRect,
                   double timelineSec, double frameRate,
                   const QString &clipName = QString()) const;

private:
    TimecodeBurnInSettings m_settings;
};

// Preview and export use this same lookup so showClipName cannot disagree
// when multiple visible video tracks overlap.
QString timecodeBurnInClipNameAt(const Timeline *timeline, double timelineSec);
