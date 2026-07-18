#include "TlrCompose16.h"

#include "HdrCompositeMath.h"

#include <QColor>
#include <QPainter>
#include <QtGlobal>

namespace tlrcompose16 {

QImage composeRgba64(const QVector<QImage>& placedBackToFront,
                     const QVector<double>& opacities,
                     QSize canvasSize)
{
    if (canvasSize.isEmpty())
        return QImage();

    QImage canvas64(canvasSize, QImage::Format_RGBA64_Premultiplied);
    canvas64.fill(QColor(0, 0, 0, 0));

    QPainter painter(&canvas64);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int i = 0; i < placedBackToFront.size(); ++i) {
        const QImage& placed = placedBackToFront[i];
        const double rawOpacity = i < opacities.size() ? opacities[i] : 1.0;
        const double opacity = qBound(0.0, rawOpacity, 1.0);
        if (placed.isNull() || opacity <= 0.001)
            continue;

        const QImage layer64 = placed.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        painter.setOpacity(opacity);
        painter.drawImage(0, 0, layer64);
    }
    painter.end();

    return canvas64;
}

QImage composeRgba64ToRgba8888(const QVector<QImage>& placedBackToFront,
                               const QVector<double>& opacities,
                               QSize canvasSize)
{
    return hdrcomposite::to8bit(composeRgba64(placedBackToFront, opacities, canvasSize))
        .convertToFormat(QImage::Format_RGBA8888);
}

QImage composeRgba64ToRgba8888(const QVector<QImage>& placedBackToFront,
                               QSize canvasSize)
{
    QVector<double> opacities;
    opacities.fill(1.0, placedBackToFront.size());
    return composeRgba64ToRgba8888(placedBackToFront, opacities, canvasSize);
}

} // namespace tlrcompose16
