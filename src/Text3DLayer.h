#pragma once

#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQuaternion>
#include <QSize>
#include <QString>
#include <QVector3D>

class Camera3D;

class Text3DLayer : public QObject
{
    Q_OBJECT

public:
    explicit Text3DLayer(QObject *parent = nullptr);

    void setText(const QString &text, const QFont &font);
    void setPerCharRotation(QVector3D perAxisAmount);
    void setPerCharPosition(QVector3D offset);
    void setPerCharScale(QVector3D scaleAmount);
    void setCameraDistance(double distance);
    void setRotationAnimAxis(QVector3D tumbleAxis);

    QImage renderFrame(QSize size, double time, const Camera3D &cam) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    double characterProgress(int characterIndex, double time) const;

    QString m_text;
    QFont m_font;
    QVector3D m_perCharRotation;
    QVector3D m_perCharPosition;
    QVector3D m_perCharScale;
    double m_cameraDistance = 400.0;
    QVector3D m_rotationAnimAxis;
    double m_staggerDelay = 0.08;
    double m_staggerDuration = 0.6;
};
