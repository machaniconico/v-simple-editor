#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>

struct LayerStyle {
    bool dropShadowEnabled = false;
    QColor shadowColor = QColor(0, 0, 0, 128);
    QPointF shadowOffset = QPointF(4.0, 4.0);
    double shadowBlurRadius = 4.0;
    double shadowOpacity = 0.75;

    bool strokeEnabled = false;
    QColor strokeColor = QColor(Qt::white);
    double strokeWidth = 2.0;

    bool isIdentity() const;

    QJsonObject toJson() const;
    static LayerStyle fromJson(const QJsonObject &obj);
};

namespace layerstyle {

QImage apply(const QImage &layerRgba, const LayerStyle &s);

} // namespace layerstyle
