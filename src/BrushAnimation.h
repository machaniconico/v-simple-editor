#pragma once

#include <QObject>

#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainterPath>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

enum BrushAnimationMode {
    PerCharacter,
    PerStroke
};

class BrushAnimation : public QObject
{
    Q_OBJECT

public:
    explicit BrushAnimation(QObject *parent = nullptr);

    void setText(const QString &text, const QFont &font, const QPointF &basePosition);
    void setBrushWidth(double width);
    void setMode(BrushAnimationMode mode);
    void setProgress(double progress);

    const QString &text() const { return m_text; }
    const QFont &font() const { return m_font; }
    const QPointF &basePosition() const { return m_basePosition; }
    double brushWidth() const { return m_brushWidth; }
    BrushAnimationMode mode() const { return m_mode; }
    double progress() const { return m_progress; }

    double totalLength() const;
    QImage renderFrame(QSize canvasSize, double progress) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    void rebuildGeometry();

    QString m_text;
    QFont m_font;
    QPointF m_basePosition;
    double m_brushWidth = 8.0;
    BrushAnimationMode m_mode = PerStroke;
    double m_progress = 0.0;

    QPainterPath m_fullPath;
    QVector<QPainterPath> m_characterPaths;
    QVector<double> m_characterLengths;
    double m_totalLength = 0.0;
};
