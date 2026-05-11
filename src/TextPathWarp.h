#pragma once

#include <QObject>

#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainterPath>
#include <QSize>
#include <QString>

class TextPathWarp : public QObject
{
    Q_OBJECT

public:
    explicit TextPathWarp(QObject *parent = nullptr);

    void setText(const QString &text, const QFont &font);
    void setBendDegrees(double bendDegrees);
    void setInflateAmount(double inflateAmount);
    void setTwistDegrees(double twistDegrees);
    void setTaperAmount(double taperAmount);

    QImage renderFrame(QSize size) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    void rebuildBasePath();
    QPainterPath buildTextPath() const;
    QPainterPath buildWarpedPath() const;
    bool hasActiveWarp() const;

    QString m_text;
    QFont m_font;
    QPainterPath m_basePath;
    double m_bendDegrees = 0.0;
    double m_inflateAmount = 0.0;
    double m_twistDegrees = 0.0;
    double m_taperAmount = 0.0;
};
