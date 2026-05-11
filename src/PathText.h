#pragma once

#include <QObject>

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainterPath>
#include <QSize>
#include <QString>
#include <QVector>

class PathText : public QObject
{
    Q_OBJECT

public:
    explicit PathText(QObject *parent = nullptr);

    void setPath(const QPainterPath &path);
    void setText(const QString &text, const QFont &font);
    void setBrushColor(const QColor &color);
    void setReversed(bool reversed);
    void setFirstMargin(double margin);
    void setLastMargin(double margin);
    void setPerpendicularOffset(double offset);

    QImage renderFrame(QSize size, double progress, double offset = 0.0) const;
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    struct GlyphData {
        QPainterPath path;
        double advance = 0.0;
    };

    void rebuildGlyphs();

    QPainterPath m_path;
    QString m_text;
    QFont m_font;
    QColor m_brushColor = Qt::white;
    bool m_reversed = false;
    double m_perpendicularOffset = 0.0;
    double m_firstMargin = 0.0;
    double m_lastMargin = 0.0;
    double m_pathLength = 0.0;
    double m_totalAdvance = 0.0;
    QVector<GlyphData> m_glyphs;
};
