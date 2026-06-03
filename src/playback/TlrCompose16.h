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

} // namespace tlrcompose16
