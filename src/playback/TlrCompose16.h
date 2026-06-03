#pragma once

#include <QImage>
#include <QSize>
#include <QVector>

namespace tlrcompose16 {

QImage composeRgba64ToRgba8888(const QVector<QImage>& placedBackToFront,
                               QSize canvasSize);

QImage composeRgba64ToRgba8888(const QVector<QImage>& placedBackToFront,
                               const QVector<double>& opacities,
                               QSize canvasSize);

inline bool isRgba64Format(QImage::Format f)
{
    return f == QImage::Format_RGBA64 || f == QImage::Format_RGBA64_Premultiplied;
}

} // namespace tlrcompose16
